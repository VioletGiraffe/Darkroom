# Data model & identity

[← Back to architecture index](../../ARCHITECTURE.md)

## `Library` is the stable root-bound lifetime

`MainWindow` owns one `Library` as a normal member. The stable public object holds a private `State`; each
state owns one immutable normalized root, its `MetadataStore`, and its `Catalog`. There is no global active
pointer and no `Catalog`/`MetadataStore` singleton. Persistent collaborators that span a possible root
change borrow `Library&` and resolve its current catalog/store when needed. Narrow `Catalog&` or
`MetadataStore&` borrows are for synchronous operations that cannot span `Library::setRoot()`; modal
root-bound flows may borrow `Library&` for their entire, switch-blocking lifetime.

`MainWindow::createWithInitialLibrary()` uses `Library::load()` to try the configured root. If it cannot be
loaded, it reports the problem and offers a folder picker until the user chooses a valid library or cancels;
only a valid `Library` is passed to the private `MainWindow` constructor. `main()` therefore neither owns nor
constructs a library, and an invalid saved path is a recoverable startup condition rather than a hard exit.

Library > Open library handles later switching through `Library::setRoot()`: it first flushes the current state,
then reads and validates the candidate's `catalog.json`/`labels.json` into objects, constructs a complete
candidate state from those same objects, and only then replaces the current state. Failure leaves every
current object and path untouched
and returns to the picker. On success, `MainWindow` synchronously closes players and clears the persistent
frame viewer, media grid and its pending thumbnail work, the library-specific label filter, and invalidates
queued card mutations before returning to the event loop; it then persists the normalized root and rebuilds
the view. Library switching and Settings are refused while an import/re-export loop is pumping events: those
loops hold short-lived catalog/store references and batch writers, and their encoding configuration must not
change mid-batch. Do not add another code path that writes `Settings::RootFolder`: `MainWindow`'s
`recordCurrentLibrary()` is its single writer, persisting the root and the recent-library entry together so the
two cannot drift (see [main-window.md](main-window.md)). Both the successful initial load and the switch flow
record a library through it.

## `Catalog` is the authoritative in-memory model

`Catalog` (see [catalog-and-labels.md](catalog-and-labels.md)) holds the media-item set in memory, keyed by
`MediaId`: for every item, its frame folder, its source path, its full label-id set, and (for a video) its
probed duration. It is kept
current by its own mutation API (`addMediaItem`/`removeMediaItem`/`applyRename`/`addLabel`/`removeLabel`/...), not by
re-deriving from disk on every refresh — during normal operation the filesystem is never walked (only the
on-demand integrity tool walks it; see [catalog-and-labels.md](catalog-and-labels.md)). `MetadataStore`
(below) is how the model is *persisted*, not a second source of truth; nothing treats `MetadataStore`'s
records as the catalog — callers ask `Catalog`.

- `Library::rootFolder()` (configured, default `H:/VideoFrames`) contains one **storage folder per label**, plus the
  reserved **`Photos/`** tree for owned photos (below).
- Each storage folder contains **frame folders** — one per imported video — holding the extracted frame
  images. A frame folder is what the UI shows as a card.
- **Photos** are first-class catalog items beside videos (`Catalog::MediaType`; the record's `"type"` field,
  absent = video). An **owned** photo's file lives at `<root>/Photos/<label>/<file>` (label subdir mirroring
  video storage folders, created lazily on first photo import to that label); a **referenced** photo (the
  record's `"referenced"` field) stays wherever the user keeps it — the catalog tracks it in place, never
  moves or deletes its file, and it has no storage folder (all its labels are stored ids). `"Photos"` is a
  reserved label name for this reason (see
  [catalog-and-labels.md](catalog-and-labels.md)).
- The link from a frame folder back to its source video is recorded in the catalog (`Catalog::addMediaItem`
  persists it via `MetadataStore`), not read from disk per-card.
- `★ Best` is a **virtual label**, not a folder: membership lives in `MetadataStore` under the `"labels"`
  field (keyed by `MediaId`) and is indexed by `Catalog`. Toggling Best adds/removes that label; no folder is
  moved.
- Source video files themselves live wherever the user keeps them (often a single dir located via
  `Catalog::anySourceDir`), not necessarily under the library root.

---

## `MediaId` (`src/Core/MediaId.h/.cpp`)

Stable identity for a media item's **source file** = its file name (matched case-insensitively) + byte size.

- `MediaId::fromFile(path)` — `QFileInfo` stat only (no content read): `name()` = original filename (kept
  for display), `size()` = byte size. Invalid (`isValid()==false`, `size==-1`) for a missing file.
- `key()` — canonical map/JSON key, `"<size>:<lowercased-name>"`. `operator==`/`qHash` compare size +
  case-insensitive name, consistent with `key()`.
- **Derived, never assigned** — recomputed from the file on demand, so there's no file→id registry to keep
  in sync (the de-sync trap a synthetic/stored id would have). This is why it can exist before a catalog.
- **Survives moves for free** (path isn't part of identity) — moving a frame folder (e.g. a `Catalog`
  relabel-relocate) needs no re-keying hook. **Does not survive rename** (name change → different
  `MediaId`) — this is *intentional*, not an oversight: an in-app rename goes through `Catalog::applyRename`
  (see [catalog-and-labels.md](catalog-and-labels.md)), which explicitly re-keys the metadata record.
- A `key()` collision (same name + same size) is, in practice, a genuine duplicate. Two independent layers
  catch it: `Catalog::addMediaItem` refuses to register an item whose id already names a *different* folder (see
  [catalog-and-labels.md](catalog-and-labels.md)), and `ImportDialog`'s file-relocation step separately
  detects a byte-identical duplicate at the destination path (see [import.md](import.md)).

## `LabelId` (`src/Core/LabelId.h`)

Stable identity for a **label** — a 64-bit value (`enum class LabelId : uint64_t`), never the display name.

- **Assigned, not derived** — the mirror image of `MediaId`. A label has no underlying file to recompute an id
  from, so `Catalog` mints one from a monotonic counter (`generateLabelId()` = `++_nextLabelId`), seeded on
  load to the highest id already in the registry. That is *why* labels need a persisted registry
  (`labels.json`) and media items don't.
- **Reserved low range `[0, 1000]`** for special/virtual labels: `None = 0` (no/invalid label; also the
  sidebar's "All" row), `Best = 1`. Real, user-created labels start at `1001`; `generateLabelId` never dips
  into the reserved band.
- **A scoped enum on purpose** — no implicit conversion to/from `QString` or `int`, so a label id can't be
  mistaken for a display name, a folder name, or a `MediaId`; a misuse is a compile error, not a silent bug.
  (It is also just a compact integer.)
- **Serialization crosses two forms** (see `LabelId.h`): a **JSON number** in `labels.json` and in each item's
  `"labels"` array; a native **`quint64`** in `QSettings` (the saved filter); and its **decimal-string** form
  (`toString` / `labelIdFromString`) only at the string-based UI seam — `ImportDialog` carries ids as
  strings so it can namespace not-yet-created labels as `"new:<n>"` (see [import.md](import.md)), and the
  `LabelSidebar`↔`MainWindow` drag payload is a string too. `MainWindow`'s dialog callbacks are where the two
  forms convert.

## `MetadataStore` (`src/Core/MetadataStore.h/.cpp`)

Single shared store for per-item metadata. Dumb persistence only — it has no notion of what a "label" or a
"folder" means; `Catalog` is the only caller that interprets its records.

- **Owned by `Library`**, GUI-thread only, no internal locking. Consumers receive a reference explicitly.
- Backing file: one **`catalog.json`** in `Library::rootFolder()` (so it travels with the library, e.g. on an
  external drive), keyed by `MediaId::key()`, **one record (JSON object of named fields) per item**.
  Written atomically via `QSaveFile`, indented for readability. `Library` reads it once and passes the
  validated object into `MetadataStore`; there is no second permissive parse in the store.
- **Field-granular API** (`get`/`set` a single named field, `remove` a whole record) so independent features
  share a record without clobbering each other; the store does read-modify-write internally.
- `allMediaIds()` reconstructs every recorded item from its key + stored name; `Catalog` enumerates items
  through here, not by walking the filesystem.
- `rekey(oldId, newId)` moves a whole record to a new identity, preserving every field. `Catalog::applyRename`
  calls it when an in-app rename changes the source filename (and thus the `MediaId`), so metadata (loop
  intervals, labels) follows the rename instead of being orphaned under the old id — the "re-key explicitly"
  hook the `MediaId` design anticipated by deliberately not surviving renames.
- **All writes go through `MetadataStore::Writer`** — `beginBatch()` is the only way to obtain one; the raw
  `set`/`remove`/`rekey` are private behind it, so an unbatched write is impossible by construction rather
  than by convention. A write applies to the in-memory records *immediately* (read-after-write inside a
  batch sees fresh data — `rebuildIndex` relies on this mid-`renameLabel`), but the disk write is deferred
  until the **outermost** live Writer is destroyed (they nest via a depth counter) and happens only if
  something changed — so a multi-field record update made through one Writer reaches disk as a single
  atomic `QSaveFile` write, never as a partially-updated record. The shared `JsonPersistence` writer checks
  parent-folder creation, open, the exact byte count, and commit. A failed flush leaves `_dirty` set and
  retains the error for retry instead of declaring success. A one-off single write is a temporary:
  `store.beginBatch().set(...)` flushes at the end of the expression (the player's loop intervals do
  this). What the Writer can't enforce is loop batching: n mutations without an enclosing batch still write
  n times (O(n²) bytes total) — wrap such loops in a `Catalog::BatchScope` (see
  [catalog-and-labels.md](catalog-and-labels.md)), which simply owns a Writer for its scope. Don't store a
  Writer long-term: it holds the batch open, deferring all persistence.
- **Record field semantics**: each `set()` also stamps a `"name"` field (the human filename, for debugging
  and catalog display). **`"name"` is reserved** — features must use other field names (loop intervals use
  `"intervals"`, labels use `"labels"`).
- Each feature owns its own (de)serialization (`QList<…>` ↔ `QJsonArray`); the store stays a dumb keyed
  JSON-blob store, not a god object.
- The store's root is immutable for its lifetime. Saves use that captured root, so an in-memory record set
  cannot be redirected by a later setting change; changing roots replaces `Library`'s private state.

## JSON load/save failure policy

`Core/JsonPersistence` is the one file-level JSON boundary for both `catalog.json` and `labels.json`.
`readObject` distinguishes a missing file (valid new-library state) from read failure, malformed JSON, a
non-object root, or a missing/wrong required top-level field. Parse diagnostics include the byte offset.
`Library` additionally validates every label entry's required field types and id uniqueness before Catalog
construction, because Catalog reconstructs that registry and startup may immediately seed and rewrite it.
The validated objects are moved directly into `MetadataStore`/`Catalog`, closing the former
validate-then-reparse gap that could collapse malformed data to an empty object and overwrite it.

Both stores retain failed writes as dirty state and expose their pending errors through `Library`.
`MainWindow` receives a failure callback but queues the dialog, so no RAII writer destructor presents UI or
throws. The dialog offers an immediate retry or continued in-memory work. `Library::setRoot()` flushes before
replacing state; window close retries and requires an explicit Discard choice before unsaved state can be
lost. A new/candidate library whose initial seeded `labels.json` cannot be saved is rejected before publish.
