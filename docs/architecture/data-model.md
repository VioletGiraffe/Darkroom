# Data model & identity

[← Back to architecture index](../../ARCHITECTURE.md)

## `Library` is the stable root-bound lifetime

`MainWindow` owns one `Library` as a normal member. The stable public object holds a private `State`; each
state owns one immutable normalized root, its `MetadataStore`, and its `Catalog`. The object is **immovable**
(every copy/move operation is deleted) and never rebuilt — only its `State` is replaced — which is what lets
collaborators hold a `Library&` for their whole lives. There is no global active
pointer and no `Catalog`/`MetadataStore` singleton. Persistent collaborators that span a possible root
change borrow `Library&` and resolve its current catalog/store when needed. Narrow `Catalog&` or
`MetadataStore&` borrows are for synchronous operations that cannot span `Library::setRoot()`; modal
root-bound flows may borrow `Library&` for their entire, switch-blocking lifetime.

**`setRoot()` is the only way to load a library** — the first root and every later one take the identical
path. A default-constructed `Library` is *empty* (its `State` is simply null); the flush operations answer
"nothing to do" in that state, so `setRoot()`'s flush-before-replace step works on the very first load too.
That empty state exists for exactly one reason: it lets the owner declare `Library` as a plain member and
load it in its own constructor, with no factory, no `std::optional`, and no second load path to keep in step.

`MainWindow`'s constructor loads the library **first, before building any UI** — the sidebar and grid borrow
it for their lifetimes, so there is nothing to build without one. Cancelling a required location prompt
leaves the window *unbuilt*: `main()` checks `isLibraryLoaded()` and drops it instead of showing an empty
shell, making an invalid saved path a recoverable startup condition rather than a hard exit.

Library > Open library handles later switching through `Library::setRoot()`: it first flushes the current state,
then reads and validates the candidate's `catalog.json`/`labels.json` into objects, constructs a complete
candidate state from those same objects, and only then replaces the current state. Failure leaves every
current object and path untouched and returns to the picker. Library switching and Settings are refused
while an import/re-export loop is pumping events: those
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

- `Library::rootFolder()` contains one **storage folder per label**, plus the
  reserved **`Photos/`** tree for owned photos (below).
- Each storage folder contains **frame folders** — one per imported video — holding the extracted frame
  images.
- **Photos** are first-class catalog items beside videos (`Catalog::MediaType`; the record's `"type"` field,
  absent = video). An **owned** photo's file lives at `<root>/Photos/<label>/<file>` (label subdir mirroring
  video storage folders); a **referenced** photo (the
  record's `"referenced"` field) stays wherever the user keeps it — the catalog tracks it in place, never
  moves or deletes its file, and it has no storage folder (all its labels are stored ids). `"Photos"` is a
  reserved label name for this reason (see
  [catalog-and-labels.md](catalog-and-labels.md)).
- The link from a frame folder back to its source video is recorded in the catalog, not read from disk per-card.
- `★ Best` is a **virtual label**, not a folder: membership lives in `MetadataStore` under the `"labels"`
  field (keyed by `MediaId`) and is indexed by `Catalog`. No folder is moved when toggling it.
- Source video files themselves live wherever the user keeps them, not necessarily under the library root.

---

## `MediaId` (`src/Core/MediaId.h/.cpp`)

Stable identity for a media item's **source file** = its file name (matched case-insensitively) + byte size.

- `MediaId::fromFile(path)` — `QFileInfo` stat only (no content read); invalid for a missing file.
- `key()` — canonical map/JSON key, `"<size>:<lowercased-name>"`. The key fold is `toLower()` and must stay
  so because `key()` is persisted — a different fold would orphan existing records.
- **Derived, never assigned** — recomputed from the file on demand, so there's no file→id registry to keep
  in sync (the de-sync trap a synthetic/stored id would have).
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
  from, so `Catalog` mints one from a monotonic counter, seeded on load to the highest id already in the
  registry. That is *why* labels need a persisted registry
  (`labels.json`) and media items don't.
- **Reserved low range `[0, 1000]`** for special/virtual labels: `None = 0` (no/invalid label), `Best = 1`.
  Real, user-created labels start at `1001`.
- **A scoped enum on purpose** — no implicit conversion to/from `QString` or `int`, so a label id can't be
  mistaken for a display name, a folder name, or a `MediaId`; a misuse is a compile error, not a silent bug.
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
  calls it when an in-app rename changes the source filename (and thus the `MediaId`), so all metadata
  follows the rename instead of being orphaned — the "re-key explicitly"
  hook the `MediaId` design anticipated by deliberately not surviving renames.
- **All writes go through `MetadataStore::Writer`** — `beginBatch()` is the only way to obtain one; the raw
  `set`/`remove`/`rekey` are private behind it, so an unbatched write is impossible by construction rather
  than by convention. A write applies to the in-memory records *immediately* (read-after-write inside a
  batch sees fresh data — `rebuildIndex` relies on this mid-`renameLabel`), but the disk write is deferred
  until the **outermost** live Writer is destroyed (they nest via a depth counter) — so a multi-field record
  update made through one Writer reaches disk as a single atomic `QSaveFile` write, never as a
  partially-updated record. A failed flush retains the error for retry instead of declaring success. What
  the Writer can't enforce is loop batching: n mutations without an enclosing batch still write
  n times (O(n²) bytes total) — wrap such loops in a `Catalog::BatchScope` (see
  [catalog-and-labels.md](catalog-and-labels.md)), which simply owns a Writer for its scope. Don't store a
  Writer long-term: it holds the batch open, deferring all persistence.
- **Record field semantics**: each `set()` also stamps a `"name"` field (the human filename, for debugging
  and display). **`"name"` is reserved** — all other features must use distinct field names.
- Each feature owns its own (de)serialization (`QList<…>` ↔ `QJsonArray`); the store stays a dumb keyed
  JSON-blob store, not a god object.
- The store's root is immutable for its lifetime. Saves use that captured root, so an in-memory record set
  cannot be redirected by a later setting change; changing roots replaces `Library`'s private state.

## JSON load/save failure policy

`Core/JsonPersistence` is the one file-level JSON boundary for both `catalog.json` and `labels.json`.
`readObject` distinguishes a missing file (valid new-library state) from a read or parse failure. `Library`
additionally validates every label entry's required field types and id uniqueness before Catalog
construction, because Catalog reconstructs that registry and startup may immediately seed and rewrite it.
The validated objects are moved directly into `MetadataStore`/`Catalog`, closing the former
validate-then-reparse gap that could collapse malformed data to an empty object and overwrite it.

Both stores retain failed writes as dirty state and expose their pending errors through `Library`.
`MainWindow` receives a failure callback but queues the dialog, so no RAII writer destructor presents UI or
throws. A new/candidate library whose initial seeded `labels.json` cannot be saved is rejected before publish.
