# Data model and identity

[← Back to architecture index](../../ARCHITECTURE.md)

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
metadata are stored by `MediaId`; they do not create directories.

## MediaId

`MediaId` identifies a source file by case-insensitive filename plus byte size. Path is excluded, so moving a source
or storage product preserves identity. Renaming the source changes identity and must go through
`Catalog::applyRename` so every persisted field is re-keyed.

`MediaId::fromFile` stats an existing file; it cannot reconstruct an id from a path after that file has moved or
disappeared. Workflows therefore capture the id while the source exists and carry it across later operations.

The canonical persisted key combines size with the lowercased filename. Its folding rule is a storage-format
contract: changing it would orphan existing records. Name-and-size identity can collide for different bytes, so
staging and registration treat such collisions explicitly rather than assuming content equality.

Media ids are derived rather than assigned; there is no separate file-to-id registry to synchronize.

## LabelId

`LabelId` is an assigned 64-bit identity, never a display or folder name. Catalog mints user-label ids from a
monotonic counter restored from `labels.json`; the low range is reserved for sentinels and virtual labels, including
`None` and `Best`.

Stable ids let label rename preserve every membership association. The scoped type prevents accidental interchange
with strings, integers, or `MediaId`. Persistence uses numeric ids; conversion to text is confined to string-based UI
payloads and ImportDialog's provisional-id namespace.

## MetadataStore

`MetadataStore` is a GUI-thread, `MediaId`-keyed JSON record store owned by Library. It deliberately knows nothing
about labels, folders, playback loops, or other feature semantics. Each feature owns the serialization of its fields,
while Catalog is the sole interpreter of item and label records.

The backing `catalog.json` travels with the library root. Records are field-granular so independent features can
update one named value without replacing their neighbors. Removing the final meaningful field removes the record;
the reserved human-readable name field cannot keep an otherwise empty record alive.

Catalog enumerates persisted ids through the store rather than disk. `rekey` moves a complete record when a source
rename changes `MediaId`, preserving unrelated feature metadata.

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

Both stores save atomically and retain failed writes as dirty, retryable state. UI reporting is queued outside writer
destruction. A candidate library whose required initial registry cannot be saved is rejected before publication, and
an existing library cannot be replaced while its pending changes remain unflushed.
