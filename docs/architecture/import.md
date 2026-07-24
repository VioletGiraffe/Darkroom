# Import: the Import module, ffmpeg, Utils, ImportDialog

[← Back to architecture index](../../ARCHITECTURE.md)

## Full frame extraction — on-demand, not at import

`MainWindow::splitVideoIntoFrames` extracts a candidate complete real frame set. It's never called during
import anymore (see "Import: preview frames only" below) — `resplitVideoIntoFrames` uses it for the first
on-demand split, Re-export all, and the integrity tool's re-import of an entry whose extracted frames are gone.
`reExportAllVideos` skips photo entries when collecting its worklist — a photo has no frames, and its
"folder" is the shared `Photos/<label>` dir, which frame-folder replacement would destroy.
- The raw ffmpeg invocation lives in `Ffmpeg::splitVideoIntoFrames` (the module owns *all* ffmpeg calls now);
  `MainWindow::splitVideoIntoFrames` is a thin wrapper. It runs ffmpeg synchronously, freezing the UI during
  extraction — decided not worth making async (see [backlog](../../ARCHITECTURE.md#improvement-backlog)).
- Only `resplitVideoIntoFrames` publishes success via `Catalog::markSplitComplete`, after the new folder and
  its preview policy have reached the transaction's commit point. Extraction itself never mutates the catalog.

`MainWindow::ensureFramesSplit(id)` is the *only* place that triggers a full split on demand: if
`!Catalog::isSplitIntoFrames(id)`, it calls `resplitVideoIntoFrames(id, /*preserveExistingPreview=*/true)`.
Wired into the grid card's middle-click handler — opening the viewer is what "pays for" a video's full
split. Deliberately not wired into `CompareWindow`: triggering a synchronous multi-second extraction from a
side-effect of a multi-select action was judged too easy to hit by accident.

`MainWindow::resplitVideoIntoFrames(id, preserveExistingPreview)` transactionally replaces an existing frame
folder with rollback on failure. Old and new frame sets coexist during extraction; low peak disk usage is
not a design goal.

`preserveExistingPreview` controls the candidate's preview before commit:
- **`true`** (`ensureFramesSplit` only) — `preview/` is still fresh from import, nothing changed that
  would make it stale, so the wrapper moves it from the preserved old folder into the successful candidate.
- **`false`** (`reExportAllVideos`, integrity re-import) — the caller explicitly wants a clean start (re-export
  may follow a settings change; the frames disappeared for an unknown reason), so a fresh one is generated
  via `Ffmpeg::generatePreviewFrames`. A requested preserved preview that does not exist takes this same fallback.

Neither path is used by `Import::importVideo`'s own overwrite-existing-folder path, which can be replacing a stale
folder left behind by a completely different prior video — there's no related `preview/` worth preserving or
regenerating-in-place there; it just goes through the normal import flow from scratch.

## Import: preview frames only, full split deferred

`Import::importVideo` (`src/Import.h/.cpp`, a free-function module in the same style as `Ffmpeg`) is the
per-item import worker. It is deliberately UI-free — it never prompts or pops a message box; every outcome
comes back as an `Import::Result` (`Success` / `FolderConflict` / `Error` with a user-presentable message).
`MainWindow::importVideoBatch` stays the batch coordinator on top of it and owns all the import UI: the
app-wide `_isProcessing` lock shared with `reExportAllVideos`, the progress modal, the folder-conflict
prompt, the per-item error boxes, `Catalog::BatchScope`, and the view refresh.

It does not extract the full frame set. It creates the output folder, puts a few
permanent preview frames into `outputFolder/preview/`, then registers the video via `Catalog::addMediaItem(...,
/*splitIntoFrames=*/false)` immediately — the video appears in the grid right away (see
[media-widgets.md](media-widgets.md)), and the expensive full
extraction only happens later via `ensureFramesSplit`.

Those preview frames are normally *reused, not re-extracted*: `ImportDialog` already ran ffmpeg to build
each staged card's preview (see "Staging area" below), so import is handed each video's staging temp dir —
keyed by the stable `MediaId` so it survives relocation moving the file — and copies those frames into
`outputFolder/preview/`. A fresh extraction runs only as a fallback, when nothing staged is available. The **duration**
the Import dialog
probed while staging rides along the same handoff — passed to `importVideo` and stored on the item, so it isn't
probed a second time. On a registration collision the whole output folder (previews included) is deleted, the same as
any
other import failure.

## Photo import: `Import::importPhoto`

`Import::importPhoto(labelPhotoFolder, photoPath, mode)` is the photo counterpart to `importVideo` — equally
UI-free, taking the verified destination produced by `Catalog::photoFolderForLabel` and returning an
`Import::PhotoResult` (`Success` / `IdCollision` / `Error` + message + the
**`registeredId`** actually registered). `MainWindow::importPhotoBatch` is its coordinator (batch scope,
error boxes, view refresh), driven from the Import dialog via the `importPhotosRequested` callback.
**Copy/Move** land the file in `<root>/Photos/<label>/` (created lazily); **"leave in place" means Reference** —
the file is tracked where it is and the catalog entry is marked referenced (`Catalog::addPhoto`,
see [catalog-and-labels.md](catalog-and-labels.md)).

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
a fresh preview is needed — the fallback above, the re-split paths, and the Import dialog's staging. Thumbnail
height and JPEG quality are fixed constants, independent of the user's full-split quality. Callers pass the exact
destination folder — a frame folder's
`preview/` subdir (via `Catalog::previewDirFor`) or a staging scratch dir — so the engine itself knows nothing
of the `preview/` convention. Each call returns a per-job **`PreviewResult`** — a status plus the video's
**duration**, which the probe had to read anyway to place the timestamps, handed back so the caller can persist
it (`Catalog::durationMsForMediaItem`, see [data-model.md](data-model.md)) instead of re-probing for the number
alone. Best-effort throughout: a failed job never aborts the batch and import never gates on it succeeding —
the status just separates a probe failure (corrupt input, nothing extracted) from an extraction failure
(duration known, frames didn't write), for the caller to act on or ignore.

It also has a batch form that extracts several videos' previews at once, used by the Import dialog's staging. The
concurrency is deliberately thread-free — each ffmpeg is its own OS process, so a bounded number run together
and are waited on, all on the calling thread.

The batch is **interruptible**: it takes a `const std::atomic<bool>&` the caller sets from another thread, polled
between the slices its process waits are broken into (so a cancel lands in ~100 ms rather than after the running
ffmpeg). Cancelling kills the in-flight processes, starts no further ones, and returns `Cancelled` for every job
that hadn't already finished cleanly — the ones that *had* keep their `Ok` result, so cancelling never throws away
work already paid for. `Cancelled` is deliberately distinct from the failure statuses: it says "you stopped this",
not "this file is broken". Partial frames from a killed extraction are left in the destination folder for the
caller to remove — the engine still owns no cleanup policy.

`preview/` is a permanent, separate store once created: a later real `splitVideoIntoFrames` run never deletes
or rewrites it (it only ever lists/writes files directly in `outputFolder`, never recursing into `preview/`),
and `CatalogIntegrity::scan`'s `extractedFramesMissing` verdict is guarded by `splitIntoFrames` specifically so a
video that's legitimately still preview-only (zero real frames yet, by design) is never misreported.

`MainWindow::buildMediaCard` builds a card's thumbnail set from `<folder>/preview/` — this is what lets a
not-yet-split video still show a real thumbnail, and means the grid never has to branch on split status for
*where* to read images from. Only when `preview/` is empty does it fall back to the real frame folder, and
with neither the card renders a "No preview" placeholder. Every catalog item gets a card either way: hiding
one would strand it, since every action on an item is reached through its card.

## Other utilities

`Utils.h` (mostly inline free functions). `ffmpegPath()` / `autoDetectedFfmpegPath()` are split because the
Settings placeholder needs the detected path *without* the setting shadowing it.
`collectFilesInDirectory(dir, recursive, predicate)` is
the shared folder-to-files expansion behind the compare/import drops and the untracked/locate scans.
`forEachFolder(root, cb)` visits every `(storageFolder, folderPath)` pair — used by the integrity scan and
untracked-file discovery, not by per-card lookups.

---

## `ImportDialog` (`src/Windows/ImportDialog.h/.cpp`)

Import dialog: copy/move source files under a label.

`ImportDialog` borrows its modal lifetime's `Library&` and reads its `Catalog` directly for lookups (label
options, photo-content duplicates, the
id-tracking check, random label colors) and calls back into its host (`MainWindow`) only for host-owned
actions it can't do itself. The `Callbacks` struct is four members:
`addMediaItemsRequested` / `importPhotosRequested` (the import workers, which own the app-wide busy lock and
the progress modal), `createLabelRequested` (shared with the sidebar's create-label flow), and
`viewChanged` (the host repaint). The Best/extra-label flush and the tracked-under-label check are done
in-dialog, not through callbacks (below). Source-file relocation lives in its own **`SourceRelocation`** module
(`src/Windows/SourceRelocation.h/.cpp`),
entry point `SourceRelocation::relocateIfNeeded`.

### Duplicate detection

Four independent layers, at different points and catching different things:
- **Already-in-library at staging** (`stageMediaItems`, direct Catalog calls): every dropped file is tested
  against the current library before it can be staged or labeled, each media type by its own identity — a photo by
  content (`Catalog::findPhotoBySameContent` byte-compares it against every imported photo of the same byte size,
  *regardless of name*, so it catches renamed duplicates), a video by its name+size `MediaId`
  (`Catalog::sourcePathForMediaItem` returns the tracked source path, empty when untracked — videos carry no content
  hash, so this is the same name+size identity `addMediaItem` enforces below). The registration refusal below stays the
  authoritative backstop for anything that reaches import anyway.
- **Same-identity file at staging** (`stageMediaItems()`): a dropped file whose `MediaId` matches an
  already-staged entry (or another file in the same drop) is byte-compared against it — identical content is
  a re-drop of the same file, skipped silently; different content is refused with a warning naming both
  paths. Accepting both is not an option: `_staged` is keyed by id, and the catalog tracks at most one
  item per id anyway, so the second file would have silently overwritten the first's staging entry.
- **File-content duplicate at the relocation destination** (the `SourceRelocation` module, above): on a same-name
  destination collision, `performRelocation` treats it as a duplicate when the `MediaId`s match (name+size,
  see [data-model.md](data-model.md)) **and** a full byte comparison (`Utils.h::filesAreIdentical`) confirms
  identical content — so the rare same-name/same-size/different-content case is still classified as "files
  differ".
- **Catalog-identity collision at registration** (`Catalog::addMediaItem`, see
  [catalog-and-labels.md](catalog-and-labels.md#import-lifecycle)): refuses to register a video whose id
  already names a *different* tracked folder.

### Staging area: mirrors the main window's own label model

Dropping a file or folder onto the dialog, or `addToStaging()` (used by `MainWindow`'s own drop and to
pre-fill staging from the untracked-file scan via `openImportDialog`), calls `stageMediaItems()`. It then
extracts a temporary preview per new video path (deduplicated by `MediaId` first — see "Duplicate detection" above)
into a per-video temp dir via the batch `Ffmpeg::generatePreviewFrames`, so several videos are processed at once. That
batch runs on a
`CInterruptableThread` while a modal `QProgressDialog` holds the GUI thread's event loop, which is what makes
staging **cancellable** — the Cancel button sets the thread's flag, and videos whose previews did finish are
still staged. A staged *photo* skips preview extraction (`StagedEntry::tempPreviewDir` stays empty) and
stages instantly. Each staged item is tracked by a `StagedEntry` (its path, temp preview dir, probed duration, pending
Best/labels, grid item), keyed by `MediaId` computed once at stage time while the source file still exists (see
"Why `MediaId`, not path" below). The temp dir is deleted once the entry is unstaged or the dialog closes — but
its frames aren't wasted: a successful Import copies them into the permanent `outputFolder/preview/` rather than
re-running ffmpeg (the import-side reuse in "Import: preview frames only" above).

### Label assignment: drag-from-list or per-card checklist, no "destination" UI

Provisional labels exist **only in the dialog** until Import — minted in-dialog (folder-derived or via
**Create label**) and never touching the Catalog. Folder-derived provisionals are revalidated when
materialized, before any backing path is created. Name matching is case-insensitive throughout, matching the
Catalog/filesystem rule. Nothing reaches the Catalog until Import — the Best star and every label toggle
mutate `pendingBest` / `pendingLabelIds` only. **Rename** renames the file on disk and re-keys the
`StagedEntry` to the new name+size `MediaId`, preserving the `staged key == the file's MediaId` invariant
`runImport` relies on (below).

A staged video needs exactly one frame folder, and that folder's name is implied by whichever label landed
on it *first* — `pendingLabelIds.constFirst()`, used only inside `runImport()` below. This is deliberately
never surfaced: no "destination" badge, no separate menu entry, no field with that name — it's purely the
order labels happen to sit in inside the `QStringList`. A label dropped (or checked) after the first is
just an ordinary additional tag, applied the same way Best is.

### `runImport()` (the "Import" button)

0. **`materializeUsedProvisionalLabels()`** runs first — the *only* point provisional labels reach the Catalog.
   Rewrites every staged entry's `pendingLabelIds` from provisional stand-in to real id; a failed creation
   leaves that item staged but unlabeled. Successfully-created provisionals are removed from `_provisionalLabels`
   so a partial Import that leaves items staged won't recreate them on a second run.
1. Groups staged entries by first label id; entries with no label are left staged. It never reconstructs a
   destination from the display name — always asks `Catalog::storageFolderForLabel` / `photoFolderForLabel`
   for a verified path.
2. **Photos in the group** (`importPhotoGroup`) go through `importPhotosRequested` (→ `MainWindow::importPhotoBatch` →
   `Import::importPhoto`, see "Photo import" above). An `IdCollision` result (Reference mode's unresolvable
   name+size clash) triggers the "import an owned copy instead?" escape hatch, retrying through the Copy
   path. Best/extra-label bookkeeping re-keys to `PhotoResult::registeredId` — an owned-import auto-rename
   gives the copy a new identity.
3. **Videos in the group** (`importVideoGroup`): relocates source files if enabled
   (`SourceRelocation::relocateIfNeeded`), then calls `addMediaItemsRequested` (`importVideoBatch`/`Import::importVideo`
   — also where a file dropped on the main window ends up, since a drop just opens this dialog pre-staged),
   passing each video's staging temp dir so import can reuse the already-extracted preview frames (see "Import:
   preview frames only" above). `isTrackedUnderLabel(id, labelId)` checks the item's tracked frame folder
   against `<storageFolder>/<source base name>`, not just "tracked at all" — so both a declined/failed import
   and a name+size collision with an item tracked elsewhere are correctly left staged rather than misread as
   successes. The staged path is updated to the relocated location when the file was actually moved, so a
   retry starts from where the file really is.
4. Both group importers accumulate into one `ImportOutcome`. Once every group is processed, `runImport` flushes
   Best flags and extra-label picks to the Catalog — guarded by `Catalog::containsMediaItem` (a
   frame-folder-exists check wouldn't work for photos), wrapped in one `Catalog::BatchScope`
   (see [catalog-and-labels.md](catalog-and-labels.md)) so a multi-item Import session writes the store once.
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
run, so re-deriving the id from the path there would return an invalid id matching nothing. `MediaId::name()`
still gives the original filename for string-only needs (e.g. deriving
`<storageFolder>/<baseName>` without touching disk) — see [data-model.md](data-model.md).

---

`CompareWindow` — side-by-side compare of the multi-selected items' frames.

`crashhandler/` — crash reporting.
