# Import and frame extraction

[← Back to architecture index](../../ARCHITECTURE.md)

## On-demand full-frame extraction

`FrameExtraction` owns first-time full-frame extraction, transactional frame-folder replacement, preview repair,
and the video-only Re-export all workflow. Import deliberately creates only a permanent preview and registers the
video as not yet split; an explicit frame-viewing action pays for the full extraction later.

`FrameExtraction::reextractVideoFrames` builds a replacement beside the current frame set and publishes
`Catalog::markSplitComplete` only after the candidate is ready to commit. Failure rolls back to the previous
folder. Old and new frame sets therefore coexist during extraction; minimizing peak disk usage is not a design goal.

`PreviewHandling` defines what the replacement transaction does with the permanent preview:

- **`PreserveExisting`** moves the imported preview into the successful first full split.
- **`Regenerate`** creates a fresh preview for explicit re-export and integrity repair. Preservation also falls
  back to regeneration if no existing preview is available.

Re-export applies only to videos: a photo has no frame set, and its storage directory may be shared by every photo
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
`splitIntoFrames == false`. The dialog normally supplies the scratch preview and duration already produced during
staging, so import copies the frames and avoids probing the video twice. Missing scratch output falls back to fresh
preview generation. A registration failure removes the newly prepared output directory.

### Photo import

Photo import uses the label's verified `Photos/<label>/` directory for owned files:

- **Copy/Move** creates or adopts an owned file there. Destination selection avoids both path collisions and
  name-and-size `MediaId` collisions, auto-renaming when necessary. A byte-identical file already at the destination
  is adopted rather than copied again.
- **Reference** tracks the source in place. Because it cannot rename an external file, an identity collision is
  refused; the interactive layer can retry as an owned copy.

The worker returns the identity actually registered because an owned import may have been renamed. Best and
additional-label bookkeeping must use that returned identity. A referenced photo receives its first label explicitly;
an owned photo derives it from its storage directory.

## Preview generation and storage

`Ffmpeg::generatePreviewFrames` is the shared fresh-preview engine used by staging, import fallback, full-split
replacement, and repair. The caller supplies the destination, so the ffmpeg layer owns neither the permanent
`preview/` convention nor scratch-directory cleanup. Each result includes extraction status and the duration already
obtained while choosing frame timestamps.

Batch generation runs a bounded number of ffmpeg subprocesses concurrently and is cancellable. Cancellation stops
in-flight work, starts no new jobs, preserves completed results, and reports unfinished jobs as cancelled. The caller
removes partial output.

Once imported, `preview/` is a permanent store separate from real extracted frames. Full extraction does not rewrite
it. Integrity scanning reports missing real frames only for items marked as fully split, so a preview-only video is a
valid catalog state. Media cards prefer the permanent preview and fall back to real frames when it is absent; see
[media-widgets.md](media-widgets.md).

## ImportDialog

`ImportDialog` borrows `Library&` for its modal lifetime and owns interactive staging plus pending import state.
It reads the current catalog for identity and label lookups, delegates source relocation to `SourceRelocation`, and
delegates per-type batch execution to `ImportExecution`. Catalog mutations are wrapped in one
`Catalog::ChangeBatchScope`, so the browser observes only the completed import result through
`Library::catalogChanged`.

### Duplicate boundaries

Duplicate handling has four distinct boundaries:

1. **Current library at staging:** photos use byte content identity, independent of name; videos use their
   name-and-size `MediaId`.
2. **Current staging set:** matching `MediaId` values are byte-compared. Identical content is a repeated drop;
   different content is refused because both items cannot occupy the same staged/catalog identity.
3. **Source-relocation destination:** a same-name destination is considered a duplicate only when identity and full
   byte content both match.
4. **Catalog registration:** the catalog remains the authoritative backstop and refuses a video identity already
   registered under a different tracked folder.

### Staging and pending labels

A drop expands supported files recursively. Folder drops retain each photo or video's hierarchy-derived label name as
a pending suggestion. **Suggest labels** consumes those suggestions on demand, reusing existing labels by
case-insensitive name and creating session-provisional labels when needed. Labels already assigned manually keep
their order.

Staged videos receive cancellable scratch previews; photos stage directly from their source image. Completed scratch
previews and probed durations are reused by video import, while unstaging or closing the dialog removes leftovers.

Provisional labels exist only in the dialog until Import. Folder suggestions and **Create label** use the same pending
model; only provisionals assigned to an item are materialized. The first pending label is the item's storage
destination, while later labels are ordinary additional tags. Destination is deliberately represented by label order
rather than separate UI or state.

Each staged entry is keyed by a `MediaId` captured while the source exists. Renaming a staged file re-keys the entry;
moving a source during import keeps the captured identity instead of deriving it again from the now-absent old path.
Every post-relocation catalog lookup and metadata assignment follows that invariant.

### Import transaction

Import first materializes used provisional labels and rewrites pending ids to their catalog ids. It then groups
labeled entries by their first label and runs the photo and video coordinators. Unlabeled entries and ordinary
failures remain staged for correction or retry.

Photo results re-key pending metadata to the identity actually registered. Video relocation updates the staged source
path so a retry follows a moved file, and a video counts as successful only when the catalog tracks it under the
expected destination label. After all groups finish, the dialog applies Best and additional labels to successfully
registered items, removes completed or explicitly skipped entries from staging, and releases the catalog-change batch
as one notification.
