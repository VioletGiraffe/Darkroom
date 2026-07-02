# Catalog & labels

[← Back to architecture index](../../ARCHITECTURE.md)

`Catalog` (`src/Core/Catalog.h/.cpp`) is **the authoritative in-memory model of the video set**, keyed by
`VideoId`, plus the **label model** layered over it. For every video it holds its frame folder, source-video
path, and full label-id set (`QHash<VideoId, Entry>`); every video-set/label query is answered from this
in-memory model, not by walking disk. Where `MetadataStore` is dumb `VideoId`-keyed byte persistence,
`Catalog` knows what a video and a label *are*: it owns the **label registry** (`labels.json`), the
`"labels"` field's id-list (de)serialization, and the model's lifecycle. Callers talk to `Catalog`, never to
`MetadataStore`, for anything video- or label-related. Meyers singleton, GUI-thread.

This replaced the original one-folder-per-collection model: collections are now dynamic in-app **filters by
label** (a video can have many; `★ Best` is just another label) rather than a 1:1 video→folder mapping. A
later pass (see "Ingestion lifecycle" below) replaced the *disk-walk-per-refresh* implementation of that
model with the current in-memory one — identity is minted from a file (a stat for name+size) at exactly two
points, ingestion (`addVideo`) and the one-time legacy seed (see [data-model.md](data-model.md)); everywhere
else a video is addressed by the `VideoId` it already carries.

## The model

- **A video has a flat label set; there is no user-facing "primary" label.** One label happens to name the
  folder its frames sit in — a disk detail, not a concept the label itself knows about.
- **Labels are objects with stable ids.** `Catalog::Label { id, displayName, color }`. The `id` is a stable
  token (generated `lbl_<hex>`; the reserved `Catalog::BestLabelId == "Best"`), **never** the display name —
  so renaming a label (changing `displayName`) leaves the id and every association intact. Per-video
  membership in `MetadataStore`'s `"labels"` field is a list of **label ids**, not names.
- **A label owns nothing on disk.** It does not know about folders. It just so happens that a video's frames
  live in a collection folder whose name equals the `displayName` of one of the video's labels — a pure
  per-video **storage detail**, derived from disk, never recorded on the label and never a stored "primary"
  flag. `isVirtual()` is **derived** (`id == BestLabelId`), not a stored field: `Best` is the sole **virtual**
  label — a filter-only label that never names a storage folder.
- **`Best` can never be renamed** (`renameLabel` refuses it) — its displayName is permanently `"Best"` by
  design. No other "virtual"-kind meta-label is anticipated; anything else needed in the future is a normal
  (ordinary, folder-eligible) label.

### Design history: why this model, not the alternative

An earlier draft of this design (recorded in session memory before implementation, see "Design rationale"
below) modelled a **"folder-backed" attribute on the label itself** — i.e. a label would *know* whether it
was backed by a collection folder. That was deliberately abandoned mid-implementation in favor of the
current model, where the folder coincidence is purely a per-video runtime fact, never stored on the label.
The reasoning: a label's relationship to a folder is a *storage detail of the videos that happen to use it*,
not a property of the label as a concept — modelling it as a label attribute conflates two different things
that don't always change together (a label can lose its last folder-backed video and gain a new one without
the label itself changing identity).

## Persistence

- **Registry (`labels.json`)** in `rootFolder()`: an ordered `[{id, displayName, color}]`, plus the
  `"seededFromSourceInfo"` legacy-seed guard flag (see [data-model.md](data-model.md)). Seeded on
  `rebuildIndex()` — `Best` (pinned first) plus one label per collection a video **in the catalog model**
  actually lives in (`ensureBestAndFolderLabels`/`ensureFolderLabelExists`). This is derived from the model,
  not a disk walk, which is why an **empty collection needs an explicit `createLabel(displayName)` call** —
  there's no video to derive its label from otherwise; `MainWindow::createCollection` calls it after `mkdir`.
  Saved only when seeding actually adds an entry.
- Per-video extra labels live in `MetadataStore`'s `"labels"` field (a `QJsonArray` of label ids), keyed by
  `VideoId` — see [data-model.md](data-model.md).

## The reconciliation model

A video's labels = the label whose `displayName` names its collection folder (the model's `folder` field)
**∪** the stored label ids. The app reconciles the folder coincidence at **exactly three points, and nowhere
else**:

1. **Model→registry seed on rebuild** (`ensureBestAndFolderLabels`/`ensureFolderLabelExists`) — a collection
   a video lives in implies its label exists; only ever *adds* missing registry entries, never removes them
   (see "Known accepted gap" below).
2. **`renameLabel`** — renames the backing collection folder (if one exists), rewrites the stored `folder`
   field of every video that was under it, then updates the registry `displayName`, keeping the id.
   Associations are by id, so nothing else changes.
3. **`removeLabel`/`deleteLabel` of the label naming a folder** — relocates the affected folder(s) (see
   "Mutations" below).

Nothing else ever touches folders. This is the load-bearing invariant for the whole label system: if new
code needs to touch a folder for label-related reasons, it almost certainly belongs at one of these three
points, not bolted on elsewhere.

## API: queries vs. mutations

**Both queries and mutations are `VideoId`-anchored** — a card carries its video's id directly (`GridItem`
stores it as a member), so nothing needs to bridge folder→id at call sites anymore. A missing-source video
still has a usable (if `!isValid()`) id, so even that case stays clean.

- **Queries**: `labelsForVideo`, `videosForLabel`, `videoHasLabel`; enumeration via `allVideos`,
  `videoCount`, `labelVideoCounts` (per-label counts in one pass, for the sidebar); per-video disk facts via
  `folderForVideo` (absolute), `sourceVideoPathForVideo`, and `anySourceVideoDir` (a sensible default
  destination for relocating newly added source files — the directory of the first video whose source file is
  currently present).
- **Mutations**: `addLabel`/`removeLabel`. `addLabel` only writes the stored id list (no-op on an invalid id —
  a missing-source video can't be labeled). `removeLabel` is metadata-only too, **except** when the label
  names the video's storage folder: then it relocates that folder to the video's alphabetically-first
  remaining ordinary label, **refusing** to remove a video's last ordinary label (a video must always keep ≥1
  ordinary label).

## Ingestion lifecycle

- **`addVideo(id, sourceVideoPath, folderAbs, splitIntoFrames)`** registers a freshly ingested video. Ensures
  the collection's folder label exists. **Refuses** (returns `false`, logs a `qWarning`, leaves the existing
  entry untouched) if `id` already names a video tracked under a *different* folder — a name+size collision
  with an existing video. `splitVideoIntoFrames` (the one caller that calls this after a real, full
  extraction) treats refusal as a failure: it deletes the just-extracted frames rather than leave them on
  disk as an untracked, catalog-invisible duplicate. Re-registering the same id at its *current* folder
  (re-export re-extracting in place, or a later on-demand split flipping `splitIntoFrames` from `false` to
  `true`) is not a collision and succeeds normally, upserting the entry's fields — this is why
  **`checkForDuplicateVideos` was removed**: the catalog is one-folder-per-`VideoId` by construction, so it
  structurally cannot hold a same-source duplicate; the old disk-walking tool that looked for one always
  reported none. Catching the collision here, at registration time, is the replacement.
  `splitIntoFrames` has no default — every call site spells out `true`/`false` explicitly. `false` means only
  permanent preview frames exist yet, a full extraction is still owed; `Catalog::isSplitIntoFrames(id)`
  queries it, and `scanIntegrity`'s ghost check is guarded by it so a video that's legitimately still
  preview-only is never misreported as a ghost (see [ingestion.md](ingestion.md) for the full on-demand-split
  design).
- **`removeVideo(id)`** drops a video from the catalog entirely (used by delete-all and dedup-cleanup), so it
  doesn't linger as a ghost now that the catalog — not a disk walk — is the authoritative set.
- **`applyRename(oldId, newId, newSourceVideoPath, newFolderAbs)`** — the rename counterpart to `addVideo`:
  carries the whole `MetadataStore` record (loop intervals, labels incl. Best) from `oldId` to `newId`
  (`MetadataStore::rekey`), then records the new source path + frame folder. `oldId == newId` is allowed (the
  source file kept its name but the frame folder moved by itself). Returns false (no-op) when `newId` already
  names a *different* tracked video — the same name+size collision guard as `addVideo` — so the caller can
  undo its disk renames instead of silently overwriting that entry. The caller
  (`MainWindow::renameVideo`, see [main-window.md](main-window.md)) does the actual file/folder renames on
  disk; this just updates the model and the persisted record to match.

### Batched writes (`Catalog::BatchScope`)

Each `addVideo`/`applyRename`/`addLabel` writes through to `MetadataStore` immediately by default — fine for
a single interactive action, but a loop over many videos (the legacy seed, a drag-drop batch, re-export-all,
the Best/extra-label flush after a Quick Import session) would otherwise re-serialize the *entire* store on
every single call, making an n-video loop write O(n²) bytes overall. `Catalog::BatchScope` is an RAII guard
— construct one at the top of such a loop — that defers the physical write until the **outermost** scope is
destroyed (nests freely), collapsing the whole loop into one write. It forwards to
`MetadataStore::beginBatch()`/`endBatch()` (not meant to be called directly — see
[data-model.md](data-model.md)). Used by the multi-video loops: the legacy seed, Quick Import's batch ingest,
re-export-all, and the post-Import Best/extra-label flush.

## Registry mutations (the label objects themselves)

- **`createLabel(displayName)`** — creates a folder-backed label with this name if none exists yet. Exists
  because empty collections have no video to derive a folder-label from (see "Persistence" above);
  `createCollection` calls it explicitly after creating the on-disk folder. No-op if the name is taken.
- **`renameLabel(labelId, newName)`** — uniqueness-checked (case-insensitive); renames the backing
  collection folder if one exists, then updates `displayName` keeping the id. Refuses: Best, empty name,
  duplicate name, or a colliding/failed folder rename.
- **`setColor(labelId, color)`** — registry-only, no folder involvement.
- **`deleteLabel(labelId)`** — the destructive one. User explicitly chose **delete-and-relocate** over
  refuse-if-used (see "Design rationale" below): it relocates every video stored under the label off it
  (reusing the per-video relocate), untags any video that merely carried it as an extra, removes the
  now-empty collection folder, then drops the registry entry. **Refuses** (no-op on the registry) for:
  Best, an unknown id, if any stored-under video would be orphaned (no other ordinary label to fall back
  on), or if a relocation was blocked (e.g. a name collision) — in the last case the folder still names the
  label, so the registry entry is deliberately *kept* (a rebuild would just re-seed it from the leftover
  folder anyway; dropping it would just create churn). `deleteLabelImpact` is a const pre-flight (relocate/untag
  counts + whether anything would be orphaned) used by the confirmation dialog and to refuse orphaning up front
  with a clear message before any mutation happens.

All registry mutations persist `labels.json`.

## In-memory model

`QHash<VideoId, Entry>` where `Entry{folder (abs), sourceVideoPath, labelIds}` — this **is** the catalog; see
"Catalog is the authoritative in-memory model" in [data-model.md](data-model.md). `rebuildIndex()` re-seeds
the registry from the model (`ensureBestAndFolderLabels`) and reloads every entry fresh from
`MetadataStore::allVideoIds()` + per-id field reads. It runs at construction and inside
`renameLabel`/`deleteLabel` (many folder paths can change at once there, so a full reload is simpler than
incremental surgery) — **not** on every grid refresh. `MainWindow::refreshVideoGrid` deliberately does **not**
call it: the model is kept current by its own mutation API (`addVideo`/`removeVideo`/`applyRename`/
`addLabel`/`removeLabel`), so re-reading the store on every refresh would be redundant work and would also
re-save the registry on every sort/filter/zoom. `addLabel`/`removeLabel` patch the affected entry in place
(`refreshVideoLabels`, or a re-key for a relocate) instead of triggering any reload.

One known wart from this: the grid skips frame folders with zero images (`MainWindow::refreshVideoGrid`'s
`addCard`, hiding a ghost from a failed re-export or an externally-deleted folder), but the sidebar's
`labelVideoCounts()` reads the model directly and still counts such a ghost. Left as-is; the integrity tool
(`Catalog::scanIntegrity`, `IntegrityCheckDialog`) surfaces and lets the user reconcile it.

## UI integration

The full mutation model is wired into the UI:
- **Sidebar filter** (`LabelSidebar`): multi-select, OR-default + AND toggle; see [main-window.md](main-window.md).
- **Per-card assignment**: context-menu **Labels** checklist (add/remove) and drag-a-sidebar-label-onto-a-card
  (add-only); see [main-window.md](main-window.md) and [video-widgets.md](video-widgets.md).
- **Sidebar label management**: right-click → rename/set color/delete; see [main-window.md](main-window.md).
- **Ingestion**: `QuickImportDialog`'s label-list panel (drag-a-label-onto-a-staged-card) and per-card Labels
  checklist, mirroring the main grid's own two assignment paths above; see [ingestion.md](ingestion.md).

---

## FS-reconciliation audit (done) — findings

A dedicated audit pass re-checked every disk/`Catalog` consistency point once all the mutation paths above
existed, looking for real gaps rather than just describing the design optimistically.

### Bug found and fixed: reserved-name guard checked the wrong string

`MainWindow`'s reserved-name guard on creating a new label/collection (`BEST_COLLECTION_NAME`, used by
`createCollection`'s "+ Add label"/"New collection" validation) was still the **pre-Catalog tab name**
`"★ Best"`, never updated when Best's `Catalog` displayName became plain `"Best"` (set in
`ensureBestLabelExists`). **Reachable through completely ordinary UI usage, not external disk tampering**:
typing "Best" into the name prompt wasn't blocked. It would create a real folder named `Best`, and the next
`rebuildIndex` → `ensureFolderLabelExists("Best")` would mint a **second, distinct label** that *also*
displays as "Best" — `labelIdForFolderName`'s lookup excludes the virtual Best from its search (only
ordinary labels can name a folder), so it never recognizes the new folder as "already covered" by the real
Best. Result: two rows both reading "Best" in the sidebar (one gold/virtual, one grey/ordinary). **Fixed**
by correcting the constant's value to `"Best"`.

### Deliberate non-fix: orphaned registry entries are never pruned

`rebuildIndex`/`ensureBestAndFolderLabels` only ever *adds* registry entries for collections a video in the
model lives in; it never *removes* a label whose last video is gone. That label lingers in the sidebar with
a 0 count until manually deleted via the UI. Two ways to reach it:
- `removeVideo` (delete-all, or dedup cleanup) on a label's *last* remaining video — nothing relocates or
  untags in this path (there's nothing left to relocate), so the now-videoless label simply isn't re-derived
  on the next `rebuildIndex`, but its existing `labels.json` entry isn't removed either. Reachable through
  ordinary in-app use, not just external tampering.
- A collection folder vanishing outside the app (e.g. deleted via Explorer while the app wasn't running) —
  the model still has each video's *stored* folder field from `MetadataStore` (nothing in the app ran to
  notice the deletion), so the label is still derived with its full stored count; the cards just render as
  invisible "ghosts" in the grid (see "In-memory model" above) rather than disappearing from the sidebar.

**Kept on purpose**, not fixed:
- Auto-pruning on a label's count dropping to zero risks destroying a label's color/associations over a
  **transient** state (mid-relocation, a momentarily-unmounted drive) rather than a genuine "this label is
  done" decision. Silently discarding user data based on a moment-in-time count is a bigger risk than a
  cosmetic 0-count row.
- Matches the conservative bias already established elsewhere in `Catalog`: `removeLabel`'s refusal to
  remove a video's last ordinary label, `deleteLabel`'s refusal to orphan a video, the refusal to drop a
  registry entry that's still folder-backed after a blocked relocation — the API consistently **refuses or
  no-ops on ambiguity rather than silently deleting**.

Revisit only if orphaned labels turn out to be a real nuisance in practice — e.g. add an explicit "garbage
collect empty/orphaned labels" action the user triggers deliberately, rather than an automatic one. The
integrity tool (`Catalog::scanIntegrity`, `IntegrityCheckDialog`) would be a natural place to add this.

---

## Design rationale (the "why" behind non-obvious choices)

Captured from the original design discussion and decisions made along the way, since current-state prose
above doesn't carry the reasoning behind forks that were considered and rejected.

- **No user-facing "primary" label, ever** — settled early and never revisited. A video's labels are a flat
  set; the UI never asks "which one is primary."
- **OR is the default filter combination**, with an explicit AND toggle in the sidebar header — chosen
  because most users filtering by multiple labels want "show me anything tagged X or Y", not the stricter
  "tagged both X and Y".
- **Assignment is drag-label-onto-video** (not drag-video-onto-label) — the label row is the drag source,
  the card is the drop target. This was a deliberate directional choice, settled early and not re-litigated.
- **`labels.json` is a separate file from `MetadataStore`'s `catalog.json`**, even though both are
  JSON-on-disk — kept separate so `MetadataStore`'s "every key is a `VideoId`" invariant stays clean; the
  label registry isn't keyed by video at all, it's keyed by label id.
- **IDs are stable synthetic tokens, never the display name** — the single biggest design decision in the
  whole model. It's what makes `renameLabel` a pure metadata edit (rename the folder, update one string,
  done) instead of requiring a rewrite of every video's stored membership list. The alternative
  (display-name-as-id) was considered and rejected specifically because renaming would otherwise require
  touching every association.
- **Delete semantics: delete-and-relocate, not refuse-if-used** — an explicit choice between two options
  when `deleteLabel` was designed: (a) only allow deleting unused labels (refuse if any video is stored
  under it), or (b) relocate affected videos to another of their labels and proceed. **(b) was chosen** —
  more powerful, accepted the added complexity (confirmation dialog showing relocate/untag counts,
  orphan-refusal pre-check) in exchange for not blocking a label deletion just because it happens to still
  have content.
- **Folders are reconciled at exactly 3 points and nowhere else** — a deliberate constraint, not an
  emergent accident. Every time a new label feature was added during implementation, the design was
  re-checked against "does this need a 4th reconciliation point?" — so far, no.
- **Retiring `source_info.txt` in favor of the in-memory `VideoId`-keyed model** (this doc's current state)
  was flagged as a future direction during the original labels design and **has since been implemented** —
  kept as a historical note so the "why move off `source_info.txt`" reasoning (a missing source file
  couldn't keep a stable identity under the old per-refresh-disk-walk model) isn't lost. On-demand frame
  extraction at ingestion was flagged at the same time and **has since also been implemented** — see
  [ingestion.md](ingestion.md).

---

## Integrity tool

Tools menu → "Check catalog integrity..." (`MainWindow::checkCatalogIntegrity`) reconciles the catalog
against disk on demand — the only other place besides the legacy seed that walks disk, and only ever on
explicit user request, never part of the normal refresh path. `Catalog::scanIntegrity()` is read-only and
returns an `IntegrityReport` of three kinds of drift, each with its own resolution:
- **`RelinkCandidate`** — an orphaned placeholder (`VideoId::fromNameAndSize(name, -1)`, from when the source
  was missing at seed/ingest time) whose recorded source path now resolves to an existing file. Resolved via
  `Catalog::relinkPlaceholder`, which unions the placeholder's stored labels with any pre-existing orphaned
  record already under the real id (see [data-model.md](data-model.md)'s "Legacy seed" note) and refuses if
  the real id already names a video tracked under a *different* folder.
- **`UntrackedFolder`** — a frame folder on disk with no catalog entry (including duplicate frame-folders the
  legacy seed skipped), optionally carrying a candidate source path recovered from a legacy
  `source_info.txt`. Resolved via `Catalog::addVideo` (registering it, `splitIntoFrames=true` since real
  frames are already on disk by definition of being found this way).
- **`GhostEntry`** — a catalog entry whose frame folder has no real images left (deleted externally, or a
  failed re-export); never reported for a video that's merely awaiting its on-demand split (guarded by
  `splitIntoFrames`, see "Ingestion lifecycle" above). Resolved either by re-importing (re-extracting frames
  if the source is still present, via `MainWindow::resplitVideoIntoFrames`) or by dropping the entry outright
  via `Catalog::removeVideo`.

`IntegrityCheckDialog::scanAndShowUi` owns all the scan/UI logic behind one static entry point; `MainWindow`
only supplies the four resolution callbacks (relink/register/reimport/remove-ghost) that actually touch the
`Catalog`/disk, plus a manual "browse to source" path for any finding the tool couldn't resolve on its own.
Everything in the "deliberate non-fix"/"known wart" notes above defers to this tool for actual reconciliation.

---

## Deferred polish (post-v1)

Two items originally deferred out of the labels feature's v1 scope to keep it tight; the first is now in
progress:

1. **App-wide restyle to match the planning mockup.** **In progress (started 2026-06-27).** The mockup at
   [`docs/mockups/main-window-sidebar.html`](../mockups/main-window-sidebar.html) is the visual reference.
   Landed so far: the card footer ([video-widgets.md](video-widgets.md)); the central `Style` sheet +
   `SegmentedToggle` ([settings-and-theme.md](settings-and-theme.md)); and the sidebar restyle
   ([main-window.md](main-window.md), "LabelSidebar structure"). The non-functional roadmap and styling
   backlog (next up: the sort popover) are tracked in out-of-repo memory, per the memory-vs-repo split.
2. **Named label chips on cards with drag-out removal.** v1 shows labels on a video card as simple colored
   dots/chips (`LabelDotStrip`, see [video-widgets.md](video-widgets.md)), display-only, removed via the
   context-menu "Labels" checklist. v-next: show larger chips bearing the label name, and let the user
   remove a label by *dragging the chip off the card* — an explicit gesture chosen over a plain click to
   guard against an accidental destructive misclick.
