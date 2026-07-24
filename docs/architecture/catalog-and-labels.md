# Catalog & labels

[← Back to architecture index](../../ARCHITECTURE.md)

`Catalog` (`src/Core/Catalog.h/.cpp`) is **the authoritative in-memory model of the media-item set**, keyed by
`MediaId`, plus the **label model** layered over it. For every item it holds its storage folder, source
path, and full label-id set (`QHash<MediaId, Entry>`); every item-set/label query is answered from this
in-memory model, not by walking disk. An item is a **video or a photo** (`Catalog::MediaType`, persisted as a
`"type"` record field — absent means video, so pre-photos catalogs need no migration): a video's folder is its
frame folder; an **owned** photo's folder is the `<root>/Photos/<label>` dir its file sits in; a **referenced**
photo (`"referenced"` field, `Catalog::isReferenced`) is tracked in place — no storage folder at all, every
label a stored extra, and catalog label/removal operations never touch its file (the separate physical Delete
UI action can). Where `MetadataStore` is dumb `MediaId`-keyed byte persistence,
`Catalog` knows what an item and a label *are*: it owns the **label registry** (`labels.json`), the
`"labels"` field's id-list (de)serialization, and the model's lifecycle. Callers talk to `Catalog`, never to
`MetadataStore`, for anything item- or label-related. Owned by `Library` and borrowed explicitly; GUI-thread only.

Identity is minted from a file (a stat for name+size) at exactly one point — import (`addMediaItem`);
everywhere else an item is addressed by the `MediaId` it already carries.

## The model

- **An item has a flat label set; there is no user-facing "primary" label.** One label happens to name the
  folder its frames sit in — a disk detail, not a concept the label itself knows about.
- **Labels are objects with stable ids.** `Catalog::Label { id, displayName, color }`. The `id` is a
  `LabelId` — a 64-bit token from a monotonic counter (`Catalog::BestLabelId == LabelId::Best == 1`; real ids
  from 1001; see [data-model.md](data-model.md)), **never** the display name — so renaming a label (changing
  `displayName`) leaves the id and every association intact. Per-item membership in `MetadataStore`'s
  `"labels"` field is a list of **label ids**, not names.
- **A label owns nothing on disk.** It does not know about folders. It just so happens that an item's frames
  live in a storage folder whose name equals the `displayName` of one of the item's labels — a pure
  per-item **storage detail**, derived from disk, never recorded on the label and never a stored "primary"
  flag. `Best` is the sole **virtual** label — filter-only, it never names a storage folder.
- **`Best` can never be renamed** (`renameLabel` refuses it) — its displayName is permanently `"Best"` by
  design. No other "virtual"-kind meta-label is anticipated; anything else needed in the future is a normal
  (ordinary, folder-eligible) label.

## Persistence

- **Registry (`labels.json`)** in the library root: an ordered `[{id, displayName, color}]`. Seeded on
  `rebuildIndex()` — `Best` (pinned first) plus one label per storage folder an item **in the catalog model**
  actually lives in. This is derived from the model, not a disk walk, which is why an **empty label needs an
  explicit `createLabel(displayName)` call** — there's no item to derive its label from otherwise; that
  operation validates the name and creates the direct-child backing folder itself.
  Each `id` persists as a **JSON number** (a `LabelId`).
- Per-item extra labels live in `MetadataStore`'s `"labels"` field (a `QJsonArray` of label ids), keyed by
  `MediaId` — see [data-model.md](data-model.md).

## The reconciliation model

An item's labels = the label its storage location implies (`storageLabelNameOf`: the storage-folder segment of a
video's folder, the label-dir name of an owned photo's `Photos/<label>` folder, nothing for a referenced
photo) **∪** the stored label ids. The app reconciles the folder coincidence at **exactly three points, and
nowhere else** — photos joined the same three points, no fourth was added:

1. **Model→registry seed on rebuild** — a storage location an item lives in implies its label exists; only ever *adds*
   missing registry entries, never removes them (a 0-item label is a normal state, see "Labels persist independent of
   item count" below). Referenced photos seed nothing — their stored label ids must already exist in the registry.
2. **`renameLabel`** — renames the backing folders (up to two: the storage folder and `<root>/Photos/<label>`), rewrites
   the stored `folder` field of affected items (plus the source path of owned photos), then updates the registry
   `displayName`, keeping the id. Associations are by id, so nothing else changes.
3. **`removeLabel`/`deleteLabel` of the label naming an item's storage** — relocates the affected storage (a
   video's frame folder / an owned photo's file; see "Mutations" below).

Every disk-touching label path skips referenced photos by construction: with no storage folder they never
classify as "stored under" a label, so only their stored id list is ever edited.

Nothing else ever touches folders. This is the load-bearing invariant for the whole label system: if new
code needs to touch a folder for label-related reasons, it almost certainly belongs at one of these three
points, not bolted on elsewhere.

## API: queries vs. mutations

**Both queries and mutations are `MediaId`-anchored** — a card carries its item's id directly (`GridItem`
stores it as a member), so nothing needs to bridge folder→id at call sites anymore. A missing-source item
still carries its id (identity is the stored name+size, not whether the file is currently present), so even
that case stays clean.

- **Queries**: standard per-item and per-label lookups; `mediaItems` exposes the full `MediaId`→`Entry` map as a const
  ref — prefer it over per-field re-queries when scanning the whole set.
- **Mutations**: `addLabel`/`removeLabel`. `addLabel` only writes the stored id list (no-op on an invalid id —
  a missing-source item can't be labeled). `removeLabel` is metadata-only too, **except** when the label
  names the item's storage location: then it relocates that storage (a video's frame folder into the
  destination's storage folder; an owned photo's file into the destination's `Photos/<label>` dir, updating its
  source path too) to the item's alphabetically-first remaining ordinary label. The **≥1-ordinary-label
  invariant is uniform**: the relocate path refuses to remove a stored-under item's last ordinary label, and
  `removeLabel` equally refuses to strip the last ordinary label of a folder-less item (a referenced photo,
  where every label is stored) even though no storage rationale applies — uniformity was an explicit design
  decision.

## Import lifecycle

- **`addMediaItem(id, sourcePath, folderAbs, splitIntoFrames)`** registers a freshly imported **video**;
  **`addPhoto(id, sourcePath, labelDirAbs, referenced)`** is its photo counterpart (same collision rule,
  writes the `"type"`/`"referenced"` fields; `labelDirAbs` is empty for a referenced photo, which is
  registered label-less — the import coordinator applies its first label via `addLabel` right after, see
  [import.md](import.md)). `addMediaItem` ensures the folder label for its storage folder exists. **Refuses** (returns
  `false`, logs a `qWarning`, leaves the existing
  entry untouched) if `id` already names an item tracked under a *different* folder — a name+size collision
  with an existing item. `splitVideoIntoFrames` (the one caller that calls this after a real, full
  extraction) treats refusal as a failure: it deletes the just-extracted frames rather than leave them on
  disk as an untracked, catalog-invisible duplicate. Re-registering the same id at its *current* folder
  (re-export re-extracting in place, or a later on-demand split flipping `splitIntoFrames` from `false` to
  `true`) is not a collision and succeeds normally, upserting the entry's fields. The catalog is
  one-folder-per-`MediaId` by construction, so it structurally cannot hold a same-source duplicate — the
  collision is caught here at registration, not by any disk scan.
  `splitIntoFrames`: `false` means only permanent preview frames exist yet, a full extraction is still owed;
  `Catalog::isSplitIntoFrames(id)`
  queries it, and `CatalogIntegrity::scan`'s `extractedFramesMissing` verdict is guarded by it so a video that's
  legitimately still preview-only is never misreported (see [import.md](import.md) for the full on-demand-split
  design). It also records the video's **duration** (ms, from the import-time probe) when a positive value is
  passed; `-1` (unknown) leaves any already-stored duration intact, so a re-registration never erases it.
  `setDurationMs` backfills it onto an already-registered item for the paths that learn duration after the fact (preview
  regeneration, re-export).
- **`removeMediaItem(id)`** drops an item from the catalog entirely. The physical Delete flow calls it only
  after every required file/folder is gone; Remove from library calls it directly because leaving disk
  content in place is that command's explicit purpose. The catalog — not a disk walk — is the authoritative
  item set, so callers must make that distinction before removing the record.
- **`applyRename(oldId, newId, newSourcePath, newFolderAbs)`** — the rename counterpart to `addMediaItem`:
  carries the full `MetadataStore` record from `oldId` to `newId`, then records the new source path + frame folder.
  `oldId == newId` is allowed (the
  source file kept its name but the frame folder moved by itself). Returns false (no-op) when `newId` already
  names a *different* tracked item — the same name+size collision guard as `addMediaItem` — so the caller can
  undo its disk renames instead of silently overwriting that entry. The caller
  (`MainWindow::renameVideo` for videos, `renamePhoto` for photos, see [main-window.md](main-window.md)) does
  the actual file/folder renames on disk; this just updates the model and the persisted record to match.

### Batched writes (`Catalog::BatchScope`)

Every Catalog mutation writes the store through its own `MetadataStore::Writer` (see
[data-model.md](data-model.md)) — so a multi-field mutation (`addMediaItem`, `addPhoto`, `applyRename`,
the relocate path) hits disk as **one atomic write of the finished record state**, never
a partially-updated one, and each single mutation is one write. But a loop over many items
(a drag-drop batch, re-export-all, the Best/extra-label flush after an Import dialog session) would still
re-serialize the *entire* store once per call, making an n-item loop write O(n²) bytes overall.
`Catalog::BatchScope` is an RAII guard — construct one at the top of such a loop — that defers the physical
write until the **outermost** scope is destroyed (nests freely), collapsing the whole loop into one write.
Used by the multi-item loops: the Import dialog's batch import, re-export-all, and the post-Import Best/extra-label
flush.

## Registry mutations (the label objects themselves)

- **`createLabel(displayName, color = {}, error)`** — validates the name, creates the direct-child backing folder, and
  inserts the registry entry. **Registry-idempotent if the name is taken** (existing label keeps its color and id;
  backing folder is ensured). Returns the created-or-existing id; the Import dialog's provisional-label materialization
  keys on it; see [import.md](import.md#runimport-the-import-button).
- **`renameLabel(labelId, newName, error)`** — renames the backing folders and updates `displayName` keeping the id (see
  reconciliation point 2). Refuses with a specific reason on name, collision, path-containment, or filesystem failure.
- **`storageFolderForLabel` / `photoFolderForLabel`** — return verified direct-child paths (refusing aliases and invalid
  labels); used by import, item relocation, and empty-folder cleanup instead of composing paths from a display name.
- **`setColor(labelId, color)`** — registry-only, no folder involvement.
- **`deleteLabel(labelId)`** — see "Design rationale" for the delete-and-relocate semantic choice. Relocates items
  stored under the label, untags extras, removes now-empty backing folders, drops the registry entry. **Refuses** for:
  Best, an unknown id, any item that would be orphaned (stored-under with no other ordinary label, or a folder-less
  referenced photo carrying it as its last ordinary label), or a blocked relocation — in the last case the registry
  entry is deliberately *kept* (a rebuild would re-seed it from the leftover folder; dropping it creates churn).
  `deleteLabelImpact` is the const pre-flight used by the confirmation dialog.

All registry mutations schedule an atomic `labels.json` save through the shared checked JSON writer. A
failed save leaves the registry dirty and retryable and is aggregated by `Library` with metadata-store
failures; see [data-model.md](data-model.md#json-loadsave-failure-policy).

### Label-name and path safety

`Catalog::labelNameValidationError` is the one contract used by create, rename, and the Import dialog's
manual provisional-label editors. A name must be **one portable, safe directory component** — the usual
filesystem-invalid-name rules (empty, whitespace/control/separator/reserved characters, `.`/`..`, trailing
dot, Windows device names) plus the app-reserved `Best` (the virtual label) and `Photos` (the `<root>/Photos`
owned-photo tree).

Syntax validation is backed by `validatedDirectChildPath`: create/rename refuse a path whose resolved parent
is not the intended root or whose existing child is a symlink. This is deliberately checked at the Catalog
boundary, before `mkpath` or rename, rather than relying on every UI to concatenate paths correctly. The path
accessors apply that same verification when an existing registry label is consumed, so a legacy unsafe name
or aliased folder cannot become an import, relocation, or cleanup destination. Such registry/folder names are
not silently removed during load; they remain visible but filesystem mutations through them are refused.

## In-memory model

`QHash<MediaId, Entry>` where `Entry{folder (abs; empty for a referenced photo), sourcePath, labelIds,
splitIntoFrames, durationMs, type, referenced}` — this **is** the catalog; see [data-model.md](data-model.md).
`rebuildIndex()` re-seeds the registry from the model and reloads every entry from the store.
A folder-less record is skipped as a non-item (a legacy orphan carrying only labels) **unless** its `type`
says photo: a folder-less photo is a referenced photo, a real item tracked in place. It runs at construction
and inside `renameLabel`/`deleteLabel` — **not** on every grid refresh. `MainWindow::refreshMediaGrid`
deliberately does **not** call it: the model is kept current by its own mutation API, so re-reading the store
on every refresh would be redundant work and would also re-save the registry on every sort/filter/zoom.
`addLabel`/`removeLabel` patch the affected entry in place instead of triggering any reload.

## UI integration

See [main-window.md](main-window.md), [media-widgets.md](media-widgets.md), and [import.md](import.md) for the UI wiring
(sidebar filter, per-card assignment, sidebar label management, Import dialog assignment).

---

## Labels persist independent of item count

`rebuildIndex`/`ensureBestAndFolderLabels` only ever *add* registry entries; they never *remove* a label
whose last item is gone. This is intentional: a label is a first-class object, and **a 0-item label is a
normal, valid state** — a freshly created empty label, or a label whose items were all relabeled or
removed. Such a label stays in the sidebar (with a 0 count) until the user explicitly deletes it via
`deleteLabel`. There is deliberately no count-based auto-pruning: a transient 0 (mid-relocation, a
momentarily-unmounted drive) is not a "this label is done" signal, and discarding a label's
color/associations over a moment-in-time count would be the real bug. This matches `Catalog`'s conservative
bias elsewhere — `removeLabel`/`deleteLabel` refuse to orphan an item, and a still-folder-backed label
survives a blocked relocation — the API refuses or no-ops on ambiguity rather than silently deleting.

---

## Design rationale (the "why" behind non-obvious choices)

Captured from the original design discussion and decisions made along the way, since current-state prose
above doesn't carry the reasoning behind forks that were considered and rejected.

- **No user-facing "primary" label, ever** — settled early and never revisited. An item's labels are a flat
  set; the UI never asks "which one is primary."
- **"Folder-backed" is never a property of the label** — drafted as a label attribute, rejected
  mid-implementation: the folder tie is a storage detail of the *items* using the label, not of the label
  itself (a label can lose its last folder-backed item and gain another without changing identity), so it
  stays a per-item runtime fact.
- **`labels.json` is a separate file from `MetadataStore`'s `catalog.json`**, even though both are
  JSON-on-disk — kept separate so `MetadataStore`'s "every key is a `MediaId`" invariant stays clean; the
  label registry isn't keyed by item at all, it's keyed by label id.
- **IDs are stable synthetic tokens, never the display name** — the single biggest design decision in the
  whole model. It's what makes `renameLabel` a pure metadata edit (rename the folder, update one string,
  done) instead of requiring a rewrite of every item's stored membership list. The alternative
  (display-name-as-id) was considered and rejected specifically because renaming would otherwise require
  touching every association.
- **Delete semantics: delete-and-relocate, not refuse-if-used** — an explicit choice between two options
  when `deleteLabel` was designed: (a) only allow deleting unused labels (refuse if any item is stored
  under it), or (b) relocate affected items to another of their labels and proceed. **(b) was chosen** —
  more powerful, accepted the added complexity (confirmation dialog showing relocate/untag counts,
  orphan-refusal pre-check) in exchange for not blocking a label deletion just because it happens to still
  have content.
- **Folders are reconciled at exactly 3 points and nowhere else** — a deliberate constraint, not an
  emergent accident. Every time a new label feature was added during implementation, the design was
  re-checked against "does this need a 4th reconciliation point?" — so far, no.

---

## Integrity tool

Tools menu → "Check catalog integrity..." (`MainWindow::checkCatalogIntegrity`) reconciles the catalog
against disk on demand — the catalog model is otherwise never re-derived from disk, so this runs only on
explicit user request, never as part of the normal refresh path. It covers **videos and photos** in both
directions: broken tracked entries, and on-disk content that no catalog entry claims — a whole video frame
folder, or a stray image file under `<root>/Photos/<label>`.

The read-only scan and its report types live in a dedicated module, `CatalogIntegrity`
(`app/src/Core/CatalogIntegrity.{h,cpp}`), reasoning purely over `Catalog`'s public API plus the on-disk
layout; `Catalog` keeps only the resolution *mutations* the scan feeds, since those touch the model and
persist. **What the check actually looks for — the full state space (a video's frame folder × split flag; a
photo's owned/referenced × source presence) and the verdicts it yields — is documented authoritatively in the
banner comment atop `CatalogIntegrity::scan`, kept next to the code so the two can't drift.** This section
stays conceptual and does not restate it.

The scan reports four kinds of drift: **untracked folder** (frame folder on disk unclaimed by any entry), **untracked
photo** (image under `<root>/Photos/<label>` unclaimed by any entry), **broken video entry** (tracked video whose
on-disk product no longer matches the catalog record), and **missing photo** (tracked photo whose source file is gone).
Full verdict→resolution mapping is in the banner comment atop `CatalogIntegrity::scan`.

`IntegrityCheckDialog::scanAndShowUi` is the one static entry point. `MainWindow` supplies the resolution callbacks that
actually touch the `Catalog`/disk (browse paths for untracked folders, moved sources, moved referenced photos).
