# Import and frame extraction

[← Back to architecture index](../DARKROOM.md)

## On-demand full-frame extraction

`FrameExtraction` owns first-time full-frame extraction, transactional frame-folder replacement, preview repair,
and the video-only re-extract-all workflow. Import deliberately creates only a permanent preview and registers the
video as not yet split; an explicit frame-viewing action pays for the full extraction later.

`FrameExtraction::reextractVideoFrames` builds a replacement beside the current frame set and publishes
`Catalog::markSplitComplete` only after the candidate is ready to commit. Failure rolls back to the previous
folder. Old and new frame sets therefore coexist during extraction; minimizing peak disk usage is not a design goal.

A batch extracts one video at a time on the GUI thread behind a cancellable progress dialog: ffmpeg already
parallelizes a single extraction. The process is polled, which keeps the dialog live, and cancelling rolls back the
current video.

`PreviewHandling` defines what the replacement transaction does with the permanent preview:

- **`PreserveExisting`** moves the imported preview into the successful first full split.
- **`Regenerate`** creates a fresh preview for explicit re-extraction and integrity repair. Preservation also falls
  back to regeneration if no existing preview is available.

Re-extraction applies only to videos: a photo has no frame set, and its storage directory may be shared by every photo
under a label. Import's overwrite-conflict path does not use frame replacement because the existing directory may
belong to an unrelated stale item; it prepares a fresh import instead.

## Import workers and interactive coordination

`Import::importVideo` and `Import::importPhoto` are UI-free per-item workers. They return structured outcomes
rather than prompting. `ImportExecution` owns the interactive batch layer above them: progress, conflict decisions,
continuing past ordinary item failures, failure summaries, and catalog write batching.

The Import dialog is application-modal while an interactive import pumps events, preventing library switching or
settings changes from invalidating the operation. It invokes `ImportExecution` directly rather than routing through
`MainWindow`.

### Video import

A video import prepares its output directory and permanent `preview/`, then registers the item with
`splitIntoFrames == false`. The output directory is named from the source's name, extension and size, numbered when
another video already owns that name. The dialog normally supplies the scratch preview and duration already produced
during staging, so import copies the frames and avoids probing the video twice. Missing scratch output falls back to
fresh preview generation. A registration failure removes the newly prepared output directory.

### Photo import

Photo import uses the label's verified `Photos/<label>/` directory for owned files:

- **Copy/Move** creates or adopts an owned file there. Destination selection skips existing files and paths recorded
  for other items, auto-renaming when necessary. A byte-identical file already at the destination is adopted rather
  than copied again, or is the result when it is already tracked.
- **Reference** tracks the source in place; a source already tracked at that path is the result.

Both workers return the registered item's id, which Best and additional-label bookkeeping use. A referenced photo
receives its first label explicitly; an owned photo derives it from its storage directory.

## ffmpeg

ffmpeg is the one external runtime dependency and is never bundled. `ffmpegPath()` (`Utils.h`) returns the configured
path when it is valid, else `autoDetectedFfmpegPath()`: beside the app (beside the bundle on macOS), then PATH, then
the Homebrew and MacPorts directories, since Finder-launched apps do not inherit the shell PATH. SettingsDialog shows
the detection result as the path field's placeholder. Only `Ffmpeg.cpp` and `OscillatingPlayback` spawn it. Without
it, staged and imported videos silently get no preview; full and single-frame extraction report the failure; photos
are unaffected.

## Preview generation and storage

`Ffmpeg::generatePreviewFrames` is the shared fresh-preview engine used by staging, import fallback, full-split
replacement, and repair. The caller supplies the destination, so the ffmpeg layer owns neither the permanent
`preview/` convention nor scratch-directory cleanup. Each result includes extraction status and the duration already
obtained while choosing frame timestamps.

Batch generation runs a bounded number of ffmpeg subprocesses concurrently and is cancellable. Cancellation stops
in-flight work, starts no new jobs, preserves completed results, and reports unfinished jobs as cancelled. The caller
removes partial output. `FrameExtraction::generatePreviewFramesInteractively` runs a batch on a worker thread behind a
cancellable progress dialog; staging and the integrity repairs both use it.

Once imported, `preview/` is a permanent store separate from real extracted frames. Full extraction does not rewrite
it. Integrity scanning reports missing real frames only for items marked as fully split, so a preview-only video is a
valid catalog state. `FrameExtraction::keepPreviewOnly` returns a split video whose frames are gone to that state.
Media cards prefer the permanent preview and fall back to real frames when it is absent; see
[media-widgets.md](media-widgets.md).

## ImportDialog

`ImportDialog` borrows `Library&` for its modal lifetime and owns interactive staging plus pending import state.
It reads the current catalog for duplicate and label lookups, delegates source relocation to `SourceRelocation`, and
delegates per-type batch execution to `ImportExecution`. Catalog mutations are wrapped in one
`Catalog::ChangeBatchScope`, so the browser observes only the completed import result through
`Library::catalogChanged`.

### Duplicate boundaries

Duplicates are decided by byte content, never by name; a size match only selects the files to compare. Duplicate
handling has four distinct boundaries:

1. **Current library at staging:** a file with the same bytes as a tracked item is not staged. Tracked items are
   selected by their recorded size.
2. **Current staging set:** a file with the same bytes as a staged one is not staged again; one at a different path is
   reported.
3. **Source-relocation destination:** a same-name destination is a duplicate only when its bytes match.
4. **Catalog registration:** the authoritative backstop refuses a source path or video frame folder that another
   item already has.

### Staging and pending labels

A drop expands supported files recursively. Folder drops retain each photo or video's hierarchy-derived label name as
a pending suggestion. **Suggest labels** consumes those suggestions on demand, reusing existing labels by
case-insensitive name and creating session-provisional labels when needed. Labels already assigned manually keep
their order.

Staged videos receive cancellable scratch previews; photos stage directly from their source image. Completed scratch
previews and probed durations are reused by video import, while unstaging or closing the dialog removes leftovers.
Videos and photos stage on separate tabs, each with its own source-handling choice and Import button: photos are
referenced in place or copied/moved into the library, source videos are left in place or copied/moved to a chosen
folder. The label list and Suggest labels are shared by both tabs.
Staged cards are naturally sorted by filename in the selected A-to-Z or Z-to-A direction.
Explicit staged-source deletion uses the shared Trash-first workflow and unstages only the successfully removed
items. Photo comparison reports successful deletion back to the dialog for the same reason. Internal scratch and
transaction cleanup remains permanent so regenerable files do not accumulate in Trash.

Provisional labels exist only in the dialog until Import. Folder suggestions and **Create label** use the same pending
model; only provisionals assigned to an item are materialized. The first pending label is the item's storage
destination, while later labels are ordinary additional tags. Destination is deliberately represented by label order
rather than separate UI or state.

Each staged entry has a dialog-local id. Renaming a staged file or relocating its source during import updates only
the entry's path. An entry gets a catalog id only when import registers it.

### Import transaction

An import covers one tab. It first materializes the provisional labels that tab's entries use and rewrites pending ids
to their catalog ids. It then groups the labeled entries by their first label and runs that tab's coordinator.
Unlabeled entries and ordinary failures remain staged for correction or retry.

Pending metadata is applied to the ids the workers return. Video relocation updates the staged source path so a retry
follows a moved file. After all groups finish, the dialog applies Best and additional labels to successfully
registered items, removes completed or explicitly skipped entries from staging, and releases the catalog-change batch
as one notification.
