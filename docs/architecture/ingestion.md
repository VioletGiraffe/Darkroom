# Ingestion: ffmpeg, Utils, QuickImportDialog

[← Back to architecture index](../../ARCHITECTURE.md)

## Full frame extraction — on-demand, not at ingestion

`MainWindow::splitVideoIntoFrames` extracts a video's complete real frame set. It's never called during
ingestion anymore (see "Ingestion: preview frames only" below) — only by `ensureFramesSplit` (the first time
the user opens the frame viewer on a video that hasn't been split yet), `reExportAllVideos`, and the
integrity tool's ghost-reimport, the latter two via the `resplitVideoIntoFrames` wrapper described below.
- Runs ffmpeg synchronously, freezing the UI during extraction — decided not worth making async (see
  [backlog](../../ARCHITECTURE.md#improvement-backlog)). Frame stepping (keep every Nth frame) and the
  user-configured full-split JPEG quality apply here; preview frames (below) are separate, with their own
  fixed quality.
- On success, registers the video via `Catalog::addVideo(..., /*splitIntoFrames=*/true)` — only once frames
  actually exist, so a failed/partial run never leaves a "tracked" but empty folder behind. On an id collision
  it deletes the just-extracted frames rather than leave an untracked duplicate (collision/upsert rules live
  in [catalog-and-labels.md](catalog-and-labels.md#ingestion-lifecycle)).

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
grid card would render nothing until the next startup's backfill migration ran. `preserveExistingPreview`
picks between two ways of avoiding that:
- **`true`** (`ensureFramesSplit` only) — `preview/` is still fresh from ingestion, nothing changed that
  would make it stale, so it's not worth regenerating: the wrapper renames it out to a sibling folder before
  the delete and back after the re-split completes (success or failure), surviving untouched.
- **`false`** (`reExportAllVideos`, ghost-reimport) — the caller explicitly wants a clean start (re-export
  may follow a settings change; a ghost's frames disappeared for an unknown reason), so the old `preview/` is
  simply deleted along with everything else, and a fresh one is regenerated via `Ffmpeg::generatePreviewFrames` once
  the real split has produced at least one real frame (skipped on a failed split, which already leaves
  `outputFolder` empty via `splitVideoIntoFrames`'s own cleanup — regenerating a preview over no real content
  would be misleading).

Neither path is used by `processVideoFile`'s own overwrite-existing-folder path, which can be wiping a stale
folder left behind by a completely different prior video — there's no related `preview/` worth preserving or
regenerating-in-place there; it just goes through the normal ingestion flow from scratch.

## Ingestion: preview frames only, full split deferred

`MainWindow::processVideoFile` no longer extracts the full frame set. It creates the output folder, puts a few
permanent preview frames into `outputFolder/preview/`, then registers the video via `Catalog::addVideo(...,
/*splitIntoFrames=*/false)` immediately — the video appears in the grid right away, with a small "split
pending" badge (see [video-widgets.md](video-widgets.md)) on its card, and the expensive full extraction only
happens later via `ensureFramesSplit`. `processBatch`'s progress text reads "Adding video X/Y...", since no
full extraction happens here.

Those preview frames are normally *reused, not re-extracted*: `QuickImportDialog` already ran ffmpeg to build
each staged card's preview (see "Staging area" below), so ingestion is handed each video's staging temp dir —
keyed by the stable `VideoId` so it survives relocation moving the file — and copies those frames into
`outputFolder/preview/`. A fresh extraction runs only as a fallback, when nothing staged is available. On a
registration collision the whole output folder (previews included) is deleted, the same as any other ingestion
failure.

`Ffmpeg::generatePreviewFrames` (`src/Ffmpeg.h/.cpp`, free functions) is that extraction engine, used whenever
a fresh preview is needed — the fallback above, the re-split paths, and Quick Import's staging. Given a video's
duration (probed from ffmpeg's own output, since no `ffprobe` ships here), it picks `frameCount` evenly-spaced
timestamps across the same 10%–90% window used for real-frame sampling and pulls one downscaled thumbnail per
timestamp in a single multi-seek ffmpeg run — seeking per-output on one open input rather than spawning a
process per frame or decoding the whole video. Thumbnail height and JPEG quality are fixed constants,
independent of the user's full-split quality. Best-effort throughout: any failure just leaves `preview/` empty
or partial, and ingestion never gates on it succeeding.

It also has a batch form that extracts several videos' previews at once, used by Quick Import staging. The
concurrency is deliberately thread-free — each ffmpeg is its own OS process, so a bounded number run together
and are waited on, all on the calling thread. A video whose probe fails is skipped (its `preview/` left empty),
and a progress callback reports completions for the staging dialog's counter.

`preview/` is a permanent, separate store once created: a later real `splitVideoIntoFrames` run never deletes
or rewrites it (it only ever lists/writes files directly in `outputFolder`, never recursing into `preview/`),
and `Catalog::scanIntegrity`'s ghost check is guarded by `splitIntoFrames` specifically so a video that's
legitimately still preview-only (zero real frames yet, by design) is never misreported as a ghost.

`MainWindow::refreshVideoGrid` always builds a card's thumbnail set from `<folder>/preview/`, never from the
real frame folder directly — this is what lets a not-yet-split video still show a real thumbnail, and means
the grid never has to branch on split status for *where* to read images from. A video is hidden from the
grid only if `preview/` itself is empty.

`MainWindow::backfillMissingPreviews()` runs once at startup, before the first grid build: every already-split
legacy video (predating this feature) has real frames but no `preview/` yet, so it back-fills one via a plain
file copy — no ffmpeg needed, since real frames already exist — selecting evenly-spaced real frames and copying
them into `preview/`. Idempotent: once `preview/` exists, later startups skip it.

## Other utilities

`Utils.h` (mostly inline free functions): `rootFolder()`, `ffmpegPath()`, `openInExplorer()`,
`getSourceVideoDate(sourceVideoPath, folderPath)` (prefers a timestamp parsed from the filename —
`parseTrailingTimestamp` — so it survives moves; falls back to the source file's birth time, then the
folder's own timestamp as a last resort), `isSupportedVideoFile()`, `filesAreIdentical()` (size-gated
byte-for-byte file comparison), `IMAGE_FILE_FILTERS`, `forEachFolder(root, cb)` (visits every
`(collection, folderPath)` pair — used by the legacy seed, not by per-card lookups anymore),
`pickEvenlySpacedFrames()`, window-geometry save/restore. The source-path lookups this used to do itself
(`getSourceVideoPath`, `existingSourceVideoDir`) are gone — callers now ask `Catalog`
(`sourceVideoPathForVideo`, `anySourceVideoDir`; see [catalog-and-labels.md](catalog-and-labels.md)).

---

## `QuickImportDialog` (`src/Windows/QuickImportDialog.h/.cpp`)

Ingestion dialog: copy/move source files into a collection.

### Duplicate detection

Three independent layers, at different points and catching different things:
- **Same-identity file at staging** (`stageVideos()`): a dropped file whose `VideoId` matches an
  already-staged entry (or another file in the same drop) is byte-compared against it — identical content is
  a re-drop of the same video, skipped silently; different content is refused with a warning naming both
  paths. Accepting both is not an option: `m_staged` is keyed by id, and the catalog tracks at most one
  video per id anyway, so the second file would have silently overwritten the first's staging entry.
- **File-content duplicate at the relocation destination** (`QuickImportDialog`, this section): on a same-name
  destination collision, `performRelocation` treats it as a duplicate when the `VideoId`s match (name+size,
  see [data-model.md](data-model.md)) **and** a full byte comparison (`Utils.h::filesAreIdentical`) confirms
  identical content — so the rare same-name/same-size/different-content case is still classified as "files
  differ". The `VideoId` check is the cheap gate that short-circuits the byte read when sizes differ.
- **Catalog-identity collision at registration** (`Catalog::addVideo`, see
  [catalog-and-labels.md](catalog-and-labels.md#ingestion-lifecycle)): refuses to register a video whose id
  already names a *different* tracked folder — the structural replacement for the old disk-walking
  `checkForDuplicateVideos` tool.

### Staging area: mirrors the main window's own label model

The dialog is one big staging area (a video-card grid) plus a small label-list panel beside it — the same
`[label list | grid]` split as `MainWindow`'s `[LabelSidebar | grid]`, and the same card widget
(`VideoItemWidget`, reused unmodified). Dropping a file onto the dialog, or `addToStaging()` (used by
`FindUntrackedFilesDialog`'s "send to staging"), calls `stageVideos()`: it extracts a temporary preview per new
path (deduplicated by `VideoId` first — see "Duplicate detection" above) into a per-video temp dir via the
batch `Ffmpeg::generatePreviewFrames`, so several videos are processed
at once behind a modal progress box. Frame count is the same `Settings::PreviewFrameCount` the main grid uses
(no separate setting). Each staged video is tracked by a `StagedEntry` (its path, temp preview dir, pending
Best/labels, grid item), keyed by `VideoId` computed once at stage time while the source file still exists (see
"Why `VideoId`, not path" below). The temp dir is deleted once the entry is unstaged or the dialog closes — but
its frames aren't wasted: a successful Import copies them into the permanent `outputFolder/preview/` rather than
re-running ffmpeg (the ingestion-side reuse in "Ingestion: preview frames only" above).

### Label assignment: drag-from-list or per-card checklist, no "destination" UI

The label-list panel is populated from `Callbacks::getLabelOptions()` (`MainWindow` supplies every
non-virtual `Catalog::allLabels()` entry, with color) and supports the same drag-a-row-out gesture as
`LabelSidebar` (`DragGestureHelper` + `LabelMimeType`, mirrored line-for-line). Dragging a label onto a
staged card — or onto a multi-selection containing it, same effective-selection shape as the main grid's
own card drop — appends the label id to that card's `pendingLabelIds` (no-op if already present) and
re-renders its label dots. Right-clicking a card opens a checkable **Labels** menu (every ordinary label,
toggling `pendingLabelIds` membership — the same pattern as the main grid's own card menu) plus **Remove
from staging**; the Best star toggles `pendingBest` directly, with no Catalog write yet either.

A staged video needs exactly one frame folder, and that folder's name is implied by whichever label landed
on it *first* — `pendingLabelIds.constFirst()`, used only inside `runImport()` below. This is deliberately
never surfaced: no "destination" badge, no separate menu entry, no field with that name — it's purely the
order labels happen to sit in inside the `QStringList`. A label dropped (or checked) after the first is
just an ordinary additional tag, applied the same way Best is.

### `runImport()` (the "Import" button)

1. Groups every staged entry with a non-empty `pendingLabelIds` by its first id; entries with no label are
   skipped entirely, left staged.
2. Per group: resolves the label's display name, relocates the group's source files if relocation is enabled
   (`performRelocation`/`FileCollisionDialog`, "Duplicate detection" above, unchanged), then calls
   `addVideosRequested` — `MainWindow`'s `processBatch`/`processVideoFile` apply path (also where a file dropped
   on the main window ends up, since a drop just opens this dialog pre-staged). It passes along each video's
   staging temp dir so ingestion can reuse the already-extracted preview frames (see "Ingestion: preview frames
   only" above).
3. Per video in the group: deferred relocation (`Cancel`) or `isVideoTrackedInCollection(id, collection)`
   coming back false leaves it staged, labels intact, to retry later. That check compares the video's
   tracked frame folder against the one this import derives (`<collection>/<source base name>`), not just
   "tracked at all" — so both an ingest that was declined/failed *and* a name+size collision with a video
   tracked elsewhere (ingestion refuses those, see "Duplicate detection" above) are correctly left staged
   rather than misread as successes. The staged path is first updated to the relocated location when the
   file was actually copied/moved, so that retry starts from where the file really is (a Move deleted the
   original; `performRelocation` also short-circuits when the
   file is already at the destination, so a retry never "collides" with itself). A collision resolved as
   Skip / "Skip and Delete" unstages the entry without ingesting (the destination copy stands in for it).
   Otherwise: a pending Best goes into `bestVideosByCollection[displayName]`; any `pendingLabelIds` beyond
   the first becomes an `ExtraLabelAssignment{collectionName, videoId, labelIds}`.
4. Once every group is processed, `markBestRequested`/`assignExtraLabelsRequested` are each called once
   (covering every group's successes together) — `MainWindow` resolves each video's frame folder from its
   `VideoId` and calls `Catalog::addLabel`, guarded by `QDir::exists` exactly as before, each wrapped in its
   own `Catalog::BatchScope` (see [catalog-and-labels.md](catalog-and-labels.md)) so a multi-video Import
   session writes the store once per callback rather than once per video. Every successfully-ingested (or
   skip-resolved) entry is then unstaged.
5. If anything succeeded, `viewChanged()` fires once (`MainWindow` wires it to `refreshLibraryView()`):
   `addVideosRequested` → `processBatch` already refreshes mid-Import as each group lands, but only with
   folder labels — the Best/extra-label flush in step 4 runs *after* that, with no refresh of its own. Since
   the dialog stays open after an Import, that gap would otherwise be visible until the dialog closes.

Nothing here is deferred past the click that triggers it, so there's no `QDialog::done(int)` override and no
caller-visible close-time flush to get right — the dialog's destructor only deletes leftover temp preview
dirs, a purely local resource the caller never depends on seeing.

### Why `VideoId`, not path, addresses a video once it's mid-Import

`isVideoTrackedInCollection`, `markBestRequested`, and `ExtraLabelAssignment` all take a `VideoId`, captured once at
stage time, rather than the staged path. `VideoId::fromFile(path)` stats the file for its size — if
relocation mode is **Move**, the source has already been deleted from that path by the time step 3/4 above
run, so re-deriving the id from the path there would return an invalid id matching nothing (the bug this
shape replaced: a Move-imported video's card never unstaged, and any label beyond the first silently never
applied). `VideoId::name()` still gives the original filename for string-only needs (e.g. deriving
`<collection>/<baseName>` without touching disk) — see [data-model.md](data-model.md).

`LabelOption` (the label-list row payload, and the type `findLabelOption()` looks up) is nested directly
inside `QuickImportDialog`, not inside its `Callbacks` namespace — a qualified reference like
`Callbacks::LabelOption` will fail to compile (qualified lookup doesn't search the enclosing class); use the
unqualified `LabelOption` from inside `QuickImportDialog`'s own member functions.

---

`CompareWindow` — side-by-side compare of the multi-selected videos' frames.

`crashhandler/` — crash reporting.
