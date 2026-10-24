# Data model and identity

[← Back to architecture index](../DARKROOM.md)

## Library as the stable lifetime boundary

`MainWindow` owns one immovable `Library`. Its replaceable private state contains an immutable normalized root,
`MetadataStore`, and `Catalog`. The public object never moves or changes identity, allowing persistent
collaborators to borrow `Library&` for life while root replacement swaps the complete private state.

There is no global active-library, Catalog, or store singleton. A collaborator that may span a root change borrows
`Library&` and resolves the current component when needed. A direct `Catalog&` or `MetadataStore&` borrow is
limited to synchronous work that cannot overlap `Library::setRoot`. Modal root-bound workflows may retain
`Library&` because they prevent switching for their lifetime.

`Library::catalogChanged` is the stable invalidation seam. The active Catalog reports successful visible mutations
through an internal callback; Library forwards those changes and successful root replacement. Persistent views
connect once to Library rather than reconnecting to each private state.

`setRoot` is the only loader for initial startup and later switches. It flushes the current state, reads and validates
the candidate persistence, constructs a complete candidate, and publishes it only after every step succeeds. Failure
leaves the current root and objects untouched. A default Library has no state, allowing it to be a plain MainWindow
member while the same flush-before-replace path handles first load.

Shell-level startup, recent-library settings, and switch interaction are documented in
[main-window.md](main-window.md).

## Catalog authority and library layout

`Catalog` is the live media-item and label model; `MetadataStore` is persistence, not a second source of truth.
Normal queries and view refreshes use Catalog without scanning disk. See
[catalog-and-labels.md](catalog-and-labels.md) for its reconciliation and mutation rules.

The library layout has three relevant forms:

- Each ordinary video-storage label has a root-level directory containing one frame folder per imported video.
- Owned photos live under the reserved `Photos/<label>/` tree.
- Referenced photos and source videos remain at external source paths recorded by Catalog.

The Catalog record links storage products to their source media. Virtual-label membership and optional feature
metadata are stored by `MediaItemId`; they do not create directories.

Persisted paths under the root are root-relative, so the library folder can move; paths outside it stay absolute.
Loading rewrites any absolute path under the root to the relative form. An older-format library moved before that
rewrite still holds paths under its previous root; loading rebases them onto the current root first. In-memory paths
are always absolute.

## MediaItemId

`MediaItemId` is an assigned identity: Catalog mints a random 64-bit value at registration, and nothing about the file
changes it. A rename, a move, a located source, or an edit to the file updates the item's recorded paths or size.
Anything that refers to a tracked item across a call or time boundary carries its id.

A file's name and size are matching aids, not an identity:

- The recorded size selects the tracked items a content-duplicate check byte-compares.
- The recorded name and size re-find a moved source in a searched folder.
- An edit made outside Darkroom leaves the recorded size stale until the integrity check records it again.

Files or folders whose names differ only in letter case are unsupported on every platform: `pathComparisonKey` folds
case by design. Case-only media renames are refused on case-sensitive file systems. Qt name filters match
case-insensitively on all platforms unless `QDir::CaseSensitive` is passed.

## LabelId

`LabelId` is an assigned 64-bit identity, never a display or folder name. Catalog mints user-label ids from a
monotonic counter restored from `labels.json`; the low range is reserved for sentinels and virtual labels, including
`None` and `Best`.

Stable ids let label rename preserve every membership association. The scoped type prevents accidental interchange
with strings, integers, or `MediaItemId`. Persistence uses numeric ids; conversion to text is confined to string-based UI
payloads and ImportDialog's provisional-id namespace.

## MetadataStore

`MetadataStore` is a GUI-thread, `MediaItemId`-keyed JSON record store owned by Library. It deliberately knows nothing
about labels, folders, playback loops, or other feature semantics. Each feature owns the serialization of its fields,
while Catalog is the sole interpreter of item and label records.

The backing `catalog.json` travels with the library root. Records are field-granular so independent features can
update one named value without replacing their neighbors. Removing a record's last field removes the record.

Every record belongs to a catalog item; features write fields only for items. Catalog enumerates persisted ids through
the store rather than disk.

All mutation goes through `MetadataStore::Writer`. Changes become visible in memory immediately, while physical
save is deferred until the outermost nested Writer ends. One Writer can therefore publish a multi-field record as a
single atomic file state. Catalog batch scopes retain a Writer across multi-item loops; Writers must not be stored
long-term because they defer all persistence.

The store captures an immutable root for its lifetime. Root switching replaces the containing Library state rather
than redirecting a live record set.

## Checked JSON publication

`Core::JsonPersistence` is the file-level boundary for both `catalog.json` and `labels.json`. Reading distinguishes
an absent file, which is valid for a new library, from unreadable or malformed data. Library validates the candidate
objects before constructing Catalog and MetadataStore, then passes those same objects through without permissive
re-parsing.

`catalog.json` carries a format version. A document from before versioning was keyed by file name and size; loading
converts it, minting ids and dropping the records that were never items, and keeps the original as
`catalog.v1-backup.json` before the converted form is saved.

Both stores save atomically and retain failed writes as dirty, retryable state. UI reporting is queued outside writer
destruction. A candidate library whose required initial registry cannot be saved is rejected before publication, and
an existing library cannot be replaced while its pending changes remain unflushed.

Import, rename, relocation, and label mutations approximate transactions across filesystem operations and JSON
writes. Runtime failures have compensating cleanup; an abrupt termination between the two steps leaves catalog and
disk out of sync. Integrity checking is the recovery path, not prevention.
