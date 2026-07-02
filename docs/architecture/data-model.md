# Data model & identity

[← Back to architecture index](../../ARCHITECTURE.md)

## `Catalog` is the authoritative in-memory model

`Catalog` (see [catalog-and-labels.md](catalog-and-labels.md)) holds the media-item set in memory, keyed by
`MediaId`: for every item, its frame folder, its source path, and its full label-id set. It is kept
current by its own mutation API (`addMediaItem`/`removeMediaItem`/`applyRename`/`addLabel`/`removeLabel`/...), not by
re-deriving from disk on every refresh — the filesystem is walked in full only once, at the one-time legacy
seed described below. `MetadataStore` (below) is how the model is *persisted*, not a second source of truth;
nothing treats `MetadataStore`'s records as the catalog — callers ask `Catalog`.

- `rootFolder()` (configured, default `H:/VideoFrames`) contains one **subfolder per collection**, plus the
  reserved **`Photos/`** tree for owned photos (below).
- Each collection folder contains **frame folders** — one per imported video — holding the extracted frame
  images. A frame folder is what the UI shows as a card.
- **Photos** are first-class catalog items beside videos (`Catalog::MediaType`; the record's `"type"` field,
  absent = video). An **owned** photo's file lives at `<root>/Photos/<label>/<file>` (label subdir mirroring
  video collections, created lazily on first photo import to that label); a **referenced** photo (the
  record's `"referenced"` field) stays wherever the user keeps it — the catalog tracks it in place, never
  moves or deletes its file, and it has no storage folder (all its labels are stored ids). `"Photos"` is a
  reserved label/collection name for this reason (see
  [catalog-and-labels.md](catalog-and-labels.md)).
- The link from a frame folder back to its source video is recorded in the catalog (`Catalog::addMediaItem`
  persists it via `MetadataStore`), not read from disk per-card. `source_info.txt` files from before this
  model existed are left on disk untouched but are never read again post-seed (see "Legacy seed" below).
- `★ Best` is a **virtual label**, not a folder: membership lives in `MetadataStore` under the `"labels"`
  field (keyed by `MediaId`) and is indexed by `Catalog`. Toggling Best adds/removes that label; no folder is
  moved. Migrated on first run from the legacy path-keyed `best.txt`, renamed to
  `best.txt.pre-labels-backup` (entries whose source video is missing can't be re-keyed and are skipped).
- Source video files themselves live wherever the user keeps them (often a single dir located via
  `Catalog::anySourceDir`), not necessarily under `rootFolder()`.

### Legacy seed: `source_info.txt` → catalog (one-time)

Before `source_info.txt` existed only as a historical artifact, it was the *only* record of a frame folder's
source video, read fresh off disk on every refresh. `Catalog::seedCatalogFromSourceInfo()` migrates that
disk state into the catalog exactly once, guarded by a `"seededFromSourceInfo"` flag persisted in
`labels.json` (so it travels with the catalog onto another drive, e.g. an external disk):

- Present source file → a real (name+size) `MediaId`. Missing/unmounted source → a `MediaId::fromNameAndSize(name, -1)`
  placeholder (`isValid() == false`) so the frames still surface under their folder label, exactly as
  before — the source can be reconciled later if the integrity tool described in
  [catalog-and-labels.md](catalog-and-labels.md) is built.
- Two folders resolving to the same id (a genuine duplicate source, or two same-named missing sources
  collapsing to one placeholder) — the second is skipped with a `qWarning`, never silently overwriting the
  first.
- **A video whose source was missing at seed time but that had labels/Best from before** (stored under its
  real `size:name` id, from when the source was still present) ends up split: the seed writes a `-1:name`
  placeholder record carrying only folder+path, while the real-id record keeps the labels but has no
  `folder`, so `rebuildIndex` skips it. Net effect: the video shows its folder label only — identical to
  pre-catalog behavior for a missing-source video, so not a regression — but the extra labels are orphaned
  under the real id until the source reappears and an integrity tool relinks them (see
  [catalog-and-labels.md](catalog-and-labels.md)).
- `source_info.txt` files are deliberately **left on disk**, never read again after the seed runs once.

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
  [catalog-and-labels.md](catalog-and-labels.md)), and `QuickImportDialog`'s file-relocation step separately
  detects a byte-identical duplicate at the destination path (see [import.md](import.md)).

## `MetadataStore` (`src/Core/MetadataStore.h/.cpp`)

Single shared store for per-item metadata. Dumb persistence only — it has no notion of what a "label" or a
"folder" means; `Catalog` is the only caller that interprets its records.

- **Meyers singleton** (`MetadataStore::instance()`), GUI-thread only, no internal locking.
- Backing file: one **`catalog.json`** in `rootFolder()` (so it travels with the collection, e.g. on an
  external drive), keyed by `MediaId::key()`, **one record (JSON object of named fields) per item**.
  Written atomically via `QSaveFile`, indented for readability. Loaded once at first use.
- **Field-granular API** (`get`/`set` a single named field, `remove` a whole record) so independent features
  share a record without clobbering each other; the store does read-modify-write internally.
- `allMediaIds()` reconstructs every recorded item from its key + stored name; `Catalog` enumerates items
  through here, not by walking the filesystem.
- `rekey(oldId, newId)` moves a whole record to a new identity, preserving every field. `Catalog::applyRename`
  calls it when an in-app rename changes the source filename (and thus the `MediaId`), so metadata (loop
  intervals, labels) follows the rename instead of being orphaned under the old id — the "re-key explicitly"
  hook the `MediaId` design anticipated by deliberately not surviving renames.
- **Persistence is immediate by default, batchable on request**: `set`/`remove`/`rekey` each write the whole
  file unconditionally — fine for one-off mutations, but O(n²) total bytes written if called n times in a
  loop (each write re-serializes the *entire* store). `beginBatch()`/`endBatch()` defer the physical write
  while a batch is open, flushing once when the outermost batch closes (nesting-safe via a depth counter).
  Not meant to be called directly — construct a `Catalog::BatchScope` instead (see
  [catalog-and-labels.md](catalog-and-labels.md)), which pairs them correctly via RAII.
- **Record field semantics**: each `set()` also stamps a `"name"` field (the human filename, for debugging
  and catalog display). **`"name"` is reserved** — features must use other field names (loop intervals use
  `"intervals"`, labels use `"labels"`).
- Each feature owns its own (de)serialization (`QList<…>` ↔ `QJsonArray`); the store stays a dumb keyed
  JSON-blob store, not a god object.
- Caveat: `rootFolder()` is read once at singleton init; changing the root folder mid-session leaves the
  store pointing at the original (a root change is treated as needing a fresh start anyway).
