# Catalog & labels

[← Back to architecture index](../../ARCHITECTURE.md)

`Catalog` (`src/Core/Catalog.h/.cpp`) is **the authoritative in-memory model of the media-item set**, keyed by
`MediaId`, plus the **label model** layered over it. For every item it holds its storage folder, source
path, and full label-id set (`QHash<MediaId, Entry>`); every item-set/label query is answered from this
in-memory model, not by walking disk. An item is a **video or a photo** (`Catalog::MediaType`, persisted as a
`"type"` record field — absent means video, so pre-photos catalogs need no migration): a video's folder is its
frame folder; an **owned** photo's folder is the `<root>/Photos/<label>` dir its file sits in; a **referenced**
photo (`"referenced"` field, `Catalog::isReferenced`) is tracked in place — no storage folder at all, every
label a stored extra, and no label operation or deletion ever touches its file. Where `MetadataStore` is dumb `MediaId`-keyed byte persistence,
`Catalog` knows what an item and a label *are*: it owns the **label registry** (`labels.json`), the
`"labels"` field's id-list (de)serialization, and the model's lifecycle. Callers talk to `Catalog`, never to
`MetadataStore`, for anything item- or label-related. Meyers singleton, GUI-thread.

This replaced the original one-folder-per-collection model: collections are now dynamic in-app **filters by
label** (an item can have many; `★ Best` is just another label) rather than a 1:1 item→folder mapping. A
later pass (see "Import lifecycle" below) replaced the *disk-walk-per-refresh* implementation of that
model with the current in-memory one — identity is minted from a file (a stat for name+size) at exactly one
point, import (`addMediaItem`); everywhere else an item is addressed by the `MediaId` it already carries.

## The model

- **An item has a flat label set; there is no user-facing "primary" label.** One label happens to name the
  folder its frames sit in — a disk detail, not a concept the label itself knows about.
- **Labels are objects with stable ids.** `Catalog::Label { id, displayName, color }`. The `id` is a
  `LabelId` — a 64-bit token from a monotonic counter (`Catalog::BestLabelId == LabelId::Best == 1`; real ids
  from 1001; see [data-model.md](data-model.md)), **never** the display name — so renaming a label (changing
  `displayName`) leaves the id and every association intact. Per-item membership in `MetadataStore`'s
  `"labels"` field is a list of **label ids**, not names.
- **A label owns nothing on disk.** It does not know about folders. It just so happens that an item's frames
  live in a collection folder whose name equals the `displayName` of one of the item's labels — a pure
  per-item **storage detail**, derived from disk, never recorded on the label and never a stored "primary"
  flag. `isVirtual()` is **derived** (`id == BestLabelId`), not a stored field: `Best` is the sole **virtual**
  label — a filter-only label that never names a storage folder.
- **`Best` can never be renamed** (`renameLabel` refuses it) — its displayName is permanently `"Best"` by
  design. No other "virtual"-kind meta-label is anticipated; anything else needed in the future is a normal
  (ordinary, folder-eligible) label.

### Design history: why this model, not the alternative

An earlier draft of this design (recorded in session memory before implementation, see "Design rationale"
below) modelled a **"folder-backed" attribute on the label itself** — i.e. a label would *know* whether it
was backed by a collection folder. That was deliberately abandoned mid-implementation in favor of the
current model, where the folder coincidence is purely a per-item runtime fact, never stored on the label.
The reasoning: a label's relationship to a folder is a *storage detail of the items that happen to use it*,
not a property of the label as a concept — modelling it as a label attribute conflates two different things
that don't always change together (a label can lose its last folder-backed item and gain a new one without
the label itself changing identity).

## Persistence

- **Registry (`labels.json`)** in `rootFolder()`: an ordered `[{id, displayName, color}]`. Seeded on
  `rebuildIndex()` — `Best` (pinned first) plus one label per collection an item **in the catalog model**
  actually lives in (`ensureBestAndFolderLabels`/`ensureFolderLabelExists`). This is derived from the model,
  not a disk walk, which is why an **empty collection needs an explicit `createLabel(displayName)` call** —
  there's no item to derive its label from otherwise; `MainWindow::createCollection` calls it after `mkdir`.
  Saved only when seeding actually adds an entry. Each `id` persists as a **JSON number** (a `LabelId`).
- Per-item extra labels live in `MetadataStore`'s `"labels"` field (a `QJsonArray` of label ids), keyed by
  `MediaId` — see [data-model.md](data-model.md).

## The reconciliation model

An item's labels = the label its storage location implies (`storageLabelNameOf`: the collection segment of a
video's folder, the label-dir name of an owned photo's `Photos/<label>` folder, nothing for a referenced
photo) **∪** the stored label ids. The app reconciles the folder coincidence at **exactly three points, and
nowhere else** — photos joined the same three points, no fourth was added:

1. **Model→registry seed on rebuild** (`ensureBestAndFolderLabels`/`ensureFolderLabelExists`) — a storage
   location an item lives in implies its label exists; only ever *adds* missing registry entries, never
   removes them (a 0-item label is a normal state, see "Labels persist independent of item count" below).
   Referenced photos seed nothing — their stored label ids
   must already exist in the registry.
2. **`renameLabel`** — renames the backing folders (up to two: the collection folder and the label's
   `<root>/Photos/<label>` dir, both collision-checked before either rename runs, with a rollback if the
   second rename fails), rewrites the stored `folder` field of every item that was under them (plus the
   source path of owned photos — their file lives inside the renamed dir), then updates the registry
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

- **Queries**: `labelsForMediaItem`, `mediaItemsForLabel`, `mediaItemHasLabel`; enumeration via `allMediaItems`,
  `containsMediaItem`, `mediaItemCount`, `labelMediaItemCounts` (per-label counts in one pass, for the
  sidebar); per-item facts via `folderForMediaItem` (absolute), `sourcePathForMediaItem`, `mediaType`,
  `isReferenced`, `durationMsForMediaItem`, and `anySourceDir` (a sensible default destination for relocating newly added source
  files — the directory of the first *video* whose source file is currently present).
- **Mutations**: `addLabel`/`removeLabel`. `addLabel` only writes the stored id list (no-op on an invalid id —
  a missing-source item can't be labeled). `removeLabel` is metadata-only too, **except** when the label
  names the item's storage location: then it relocates that storage (a video's frame folder into the
  destination collection; an owned photo's file into the destination's `Photos/<label>` dir, updating its
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
  [import.md](import.md)). `addMediaItem` ensures the collection's folder label exists. **Refuses** (returns `false`, logs a `qWarning`, leaves the existing
  entry untouched) if `id` already names an item tracked under a *different* folder — a name+size collision
  with an existing item. `splitVideoIntoFrames` (the one caller that calls this after a real, full
  extraction) treats refusal as a failure: it deletes the just-extracted frames rather than leave them on
  disk as an untracked, catalog-invisible duplicate. Re-registering the same id at its *current* folder
  (re-export re-extracting in place, or a later on-demand split flipping `splitIntoFrames` from `false` to
  `true`) is not a collision and succeeds normally, upserting the entry's fields — this is why
  **`checkForDuplicateVideos` was removed**: the catalog is one-folder-per-`MediaId` by construction, so it
  structurally cannot hold a same-source duplicate; the old disk-walking tool that looked for one always
  reported none. Catching the collision here, at registration time, is the replacement.
  `splitIntoFrames` has no default — every call site spells out `true`/`false` explicitly. `false` means only
  permanent preview frames exist yet, a full extraction is still owed; `Catalog::isSplitIntoFrames(id)`
  queries it, and `CatalogIntegrity::scan`'s ghost verdict is guarded by it so a video that's legitimately still
  preview-only is never misreported as a ghost (see [import.md](import.md) for the full on-demand-split
  design). It also records the video's **duration** (ms, from the import-time probe) when a positive value is
  passed; `-1` (unknown) leaves any already-stored duration intact, so a re-registration never erases it.
  `setDurationMs(id, ms)` backfills it onto an already-registered item for the paths that learn the duration
  after the fact — preview regeneration and re-export — and `durationMsForMediaItem(id)` reads it back (`-1`
  for a photo, an unknown id, or a pre-duration import not yet backfilled).
- **`removeMediaItem(id)`** drops an item from the catalog entirely (used by delete-all and dedup-cleanup), so it
  doesn't linger as a ghost now that the catalog — not a disk walk — is the authoritative set.
- **`applyRename(oldId, newId, newSourcePath, newFolderAbs)`** — the rename counterpart to `addMediaItem`:
  carries the whole `MetadataStore` record (loop intervals, labels incl. Best) from `oldId` to `newId`
  (`MetadataStore::rekey`), then records the new source path + frame folder. `oldId == newId` is allowed (the
  source file kept its name but the frame folder moved by itself). Returns false (no-op) when `newId` already
  names a *different* tracked item — the same name+size collision guard as `addMediaItem` — so the caller can
  undo its disk renames instead of silently overwriting that entry. The caller
  (`MainWindow::renameMediaItem`, see [main-window.md](main-window.md)) does the actual file/folder renames on
  disk; this just updates the model and the persisted record to match.

### Batched writes (`Catalog::BatchScope`)

Every Catalog mutation writes the store through its own `MetadataStore::Writer` (see
[data-model.md](data-model.md)) — so a multi-field mutation (`addMediaItem`, `addPhoto`, `applyRename`,
the relocate path) hits disk as **one atomic write of the finished record state**, never
a partially-updated one, and each single mutation is one write. But a loop over many items
(a drag-drop batch, re-export-all, the Best/extra-label flush after a Quick Import session) would still
re-serialize the *entire* store once per call, making an n-item loop write O(n²) bytes overall.
`Catalog::BatchScope` is an RAII guard — construct one at the top of such a loop — that defers the physical
write until the **outermost** scope is destroyed (nests freely), collapsing the whole loop into one write.
It simply owns a Writer it never writes through; the mutations' own nested Writers coalesce into it. Used by
the multi-item loops: Quick Import's batch import, re-export-all, and the post-Import
Best/extra-label flush.

## Registry mutations (the label objects themselves)

- **`createLabel(displayName, color = {})`** — creates a folder-backed label with this name if none exists yet
  (with the given color, or a random one), and returns the created-or-existing label's id. Exists
  because empty collections have no item to derive a folder-label from (see "Persistence" above);
  `createCollection` calls it explicitly after creating the on-disk folder and passes the id through to its
  own caller (Quick Import's provisional-label materialization keys on it — see
  [import.md](import.md#runimport-the-import-button)). Registry-idempotent if the name is taken: the existing
  label keeps its color, and its id is returned all the same.
- **`renameLabel(labelId, newName)`** — uniqueness-checked (case-insensitive); renames the backing folders
  if they exist (see reconciliation point 2 above), then updates `displayName` keeping the id. Refuses: Best,
  empty name, duplicate name, the reserved name `"Photos"` (see below), or a colliding/failed folder rename.
- **`setColor(labelId, color)`** — registry-only, no folder involvement.
- **`deleteLabel(labelId)`** — the destructive one. User explicitly chose **delete-and-relocate** over
  refuse-if-used (see "Design rationale" below): it relocates every item stored under the label off it
  (reusing the per-item relocate), untags any item that merely carried it as an extra, removes the
  now-empty backing folders (the collection folder and the label's `Photos/<label>` dir, each only if
  empty), then drops the registry entry. **Refuses** (no-op on the registry) for:
  Best, an unknown id, if any item would be orphaned (a stored-under item with no other ordinary label to
  fall back on, or a folder-less referenced photo carrying this as its last ordinary label — untagging it
  would orphan it just as surely), or if a relocation was blocked (e.g. a name collision) — in the last case
  the folder still names the label, so the registry entry is deliberately *kept* (a rebuild would just
  re-seed it from the leftover folder anyway; dropping it would just create churn). `deleteLabelImpact` is a
  const pre-flight (relocate/untag counts + whether anything would be orphaned) used by the confirmation
  dialog and to refuse orphaning up front with a clear message before any mutation happens.

All registry mutations persist `labels.json`.

**`"Photos"` is a reserved name**: `<root>/Photos` is the owned-photo storage tree (one subdir per label —
see [data-model.md](data-model.md)), living in the same namespace as collection folders. `createCollection`
(UI side) and `renameLabel` (catalog side) both refuse it case-insensitively. A *legacy* collection folder
literally named `Photos` predating this rule is not auto-migrated — accepted as a non-case for this
library; the guards only prevent creating the conflict from here on.

## In-memory model

`QHash<MediaId, Entry>` where `Entry{folder (abs; empty for a referenced photo), sourcePath, labelIds,
splitIntoFrames, durationMs, type, referenced}` — this **is** the catalog; see "Catalog is the authoritative in-memory
model" in [data-model.md](data-model.md). `rebuildIndex()` re-seeds the registry from the model
(`ensureBestAndFolderLabels`) and reloads every entry fresh from `MetadataStore::allMediaIds()` + per-id
field reads. A folder-less record is skipped as a non-item (a legacy orphan carrying only labels) **unless**
its `type` says photo: a folder-less photo is a referenced photo, a real item tracked in place. It runs at construction and inside
`renameLabel`/`deleteLabel` (many folder paths can change at once there, so a full reload is simpler than
incremental surgery) — **not** on every grid refresh. `MainWindow::refreshMediaGrid` deliberately does **not**
call it: the model is kept current by its own mutation API (`addMediaItem`/`removeMediaItem`/`applyRename`/
`addLabel`/`removeLabel`), so re-reading the store on every refresh would be redundant work and would also
re-save the registry on every sort/filter/zoom. `addLabel`/`removeLabel` patch the affected entry in place
(`refreshMediaItemLabels`, or a re-key for a relocate) instead of triggering any reload.

One known wart from this: the grid skips frame folders with zero images (`MainWindow::refreshMediaGrid`'s
`addCard`, hiding a ghost from a failed re-export or an externally-deleted folder), but the sidebar's
`labelMediaItemCounts()` reads the model directly and still counts such a ghost. Left as-is; the integrity tool
(`CatalogIntegrity::scan`, `IntegrityCheckDialog`) surfaces and lets the user reconcile it.

## UI integration

The full mutation model is wired into the UI:
- **Sidebar filter** (`LabelSidebar`): multi-select, OR-default + AND toggle; see [main-window.md](main-window.md).
- **Per-card assignment**: context-menu **Labels** checklist (add/remove) and drag-a-sidebar-label-onto-a-card
  (add-only); see [main-window.md](main-window.md) and [media-widgets.md](media-widgets.md).
- **Sidebar label management**: right-click → rename/set color/delete; see [main-window.md](main-window.md).
- **Import**: `ImportDialog`'s label-list panel (drag-a-label-onto-a-staged-card) and per-card Labels
  checklist, mirroring the main grid's own two assignment paths above; see [import.md](import.md).

---

## FS-reconciliation audit (done) — findings

A dedicated audit pass re-checked every disk/`Catalog` consistency point once all the mutation paths above
existed, looking for real gaps rather than just describing the design optimistically.

### Bug found and fixed: reserved-name guard checked the wrong string

`MainWindow`'s reserved-name guard on creating a new label/collection (`BEST_COLLECTION_NAME`, used by
`createCollection`'s "Create label"/"New collection" validation) was still the **pre-Catalog tab name**
`"★ Best"`, never updated when Best's `Catalog` displayName became plain `"Best"` (set in
`ensureBestLabelExists`). **Reachable through completely ordinary UI usage, not external disk tampering**:
typing "Best" into the name prompt wasn't blocked. It would create a real folder named `Best`, and the next
`rebuildIndex` → `ensureFolderLabelExists("Best")` would mint a **second, distinct label** that *also*
displays as "Best" — `ordinaryLabelIdByName`'s lookup excludes the virtual Best from its search (only
ordinary labels can name a folder), so it never recognizes the new folder as "already covered" by the real
Best. Result: two rows both reading "Best" in the sidebar (one gold/virtual, one grey/ordinary). **Fixed**
by correcting the constant's value to `"Best"`.

### Labels persist independent of item count

`rebuildIndex`/`ensureBestAndFolderLabels` only ever *add* registry entries; they never *remove* a label
whose last item is gone. This is intentional: a label is a first-class object, and **a 0-item label is a
normal, valid state** — a freshly created empty collection, or a label whose items were all relabeled or
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
- **OR is the default filter combination**, with an explicit AND toggle in the sidebar header — chosen
  because most users filtering by multiple labels want "show me anything tagged X or Y", not the stricter
  "tagged both X and Y".
- **Assignment is drag-label-onto-item** (not drag-item-onto-label) — the label row is the drag source,
  the card is the drop target. This was a deliberate directional choice, settled early and not re-litigated.
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
- **On-demand frame extraction at import** was flagged as a future direction during the original labels
  design and **has since been implemented** — see [import.md](import.md).

---

## Integrity tool

Tools menu → "Check catalog integrity..." (`MainWindow::checkCatalogIntegrity`) reconciles the catalog
against disk on demand — the catalog model is otherwise never re-derived from disk, so this runs only on
explicit user request, never as part of the normal refresh path. It covers **videos and photos**. The one
piece still deferred is discovering an *untracked* file under `<root>/Photos/<label>` (the untracked walk
skips the `Photos` tree), so a stray photo file that no catalog entry claims isn't surfaced yet.

The read-only scan and its report types live in a dedicated module, `CatalogIntegrity`
(`app/src/Core/CatalogIntegrity.{h,cpp}`), reasoning purely over `Catalog`'s public API plus the on-disk
layout; `Catalog` keeps only the resolution *mutations* the scan feeds, since those touch the model and
persist. **What the check actually looks for — the full state space (a video's frame folder × split flag; a
photo's owned/referenced × source presence) and the verdicts it yields — is documented authoritatively in the
banner comment atop `CatalogIntegrity::scan`, kept next to the code so the two can't drift.** This section
stays conceptual and does not restate it.

The scan reports three kinds of drift, each with its own resolution:
- **Untracked folder** — a non-empty frame folder on disk that no catalog entry claims → the user browses to
  its source video to register it.
- **Broken video entry** — a tracked video whose on-disk product (preview frames and/or real frames) no
  longer matches what the catalog records → resolved by re-import, preview regeneration, marking it fully
  split, or removal, depending on which verdicts hold (the banner maps verdict → available resolution).
- **Missing photo** — a tracked photo whose source file is gone; a photo is source-only, so that's its whole
  failure mode (owned → LOST, referenced → GONE). A referenced photo offers **Locate** (browse to the moved
  file, repointing the entry through `applyRename`) plus Remove/Skip; an owned photo, which lives in the
  library tree, offers only Remove/Skip.

`IntegrityCheckDialog::scanAndShowUi` owns all the scan/UI logic behind one static entry point; `MainWindow`
only supplies the resolution callbacks that actually touch the `Catalog`/disk, including the manual "browse"
paths (an untracked folder's source video, a moved referenced photo). The catalog-vs-disk drift scenarios
described above (ghost / invisible cards, missing sources) defer to this tool for actual reconciliation.

---

## Deferred polish (post-v1)

Two items originally deferred out of the labels feature's v1 scope to keep it tight; the first is now in
progress:

1. **App-wide restyle to match the planning mockup.** **In progress (started 2026-06-27).** The mockup at
   [`docs/mockups/main-window-sidebar.html`](../mockups/main-window-sidebar.html) is the visual reference.
   Landed so far: the card footer ([media-widgets.md](media-widgets.md)); the central `Style` sheet +
   `SegmentedToggle` ([settings-and-theme.md](settings-and-theme.md)); and the sidebar restyle
   ([main-window.md](main-window.md), "LabelSidebar structure"). The non-functional roadmap and styling
   backlog (next up: the sort popover) are tracked in out-of-repo memory, per the memory-vs-repo split.
2. **Named label chips on cards with drag-out removal.** v1 shows labels on an item card as simple colored
   dots/chips (`LabelDotStrip`, see [media-widgets.md](media-widgets.md)), display-only, removed via the
   context-menu "Labels" checklist. v-next: show larger chips bearing the label name, and let the user
   remove a label by *dragging the chip off the card* — an explicit gesture chosen over a plain click to
   guard against an accidental destructive misclick.
