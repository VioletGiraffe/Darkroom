# Catalog and labels

[← Back to architecture index](../../ARCHITECTURE.md)

`Catalog` is the authoritative GUI-thread model of the library's media-item set and label registry. It is keyed by
`MediaId`, kept current through its mutation API, and owned by `Library`. Item and label queries never derive the
normal application state by walking disk. Callers use `Catalog`, not `MetadataStore`, for model semantics.

Identity is minted from the source file at import. Every later operation carries the existing `MediaId`; source
presence and storage relocation do not redefine the item's identity.

## Model and persistence

A catalog item is either a video or photo:

- A video's storage folder is its frame folder.
- An owned photo's storage folder is the shared `Photos/<label>` directory containing its file.
- A referenced photo is tracked at its external source path and has no library storage folder. Label mutations never
  touch that external file; physical deletion is a separate explicit operation.

Items have a flat set of labels with no user-facing primary label. Labels are objects with a stable synthetic id,
display name, and color. Associations persist by id, so renaming changes the display name and backing paths without
rewriting every membership record.

A label owns nothing on disk. One ordinary label happens to match each stored item's current storage directory, but
that is a property of the item location, not of the label. `Best` is the sole virtual label: it can filter items but
never names storage and cannot be renamed or deleted.

The ordered label registry lives in `labels.json`; per-item additional label ids live in `MetadataStore`. Keeping
the registry separate preserves the store's invariant that every metadata record is keyed by `MediaId`. A registry
entry persists independently of item count: an empty label is valid and remains until explicitly deleted.

## Storage-label reconciliation

An item's effective labels are the ordinary label implied by its storage location, when it has one, plus its stored
label ids. This disk/model coincidence is reconciled at exactly three points:

1. **Registry seeding:** loading the catalog adds any missing registry label implied by a modeled item's storage
   location. It never removes registry entries merely because no item currently uses them.
2. **Label rename:** the storage and owned-photo backing directories are renamed, affected item paths are updated,
   and the registry display name changes while the stable id remains.
3. **Removal of a storage-implied label:** the affected video folder or owned photo is relocated under another of the
   item's ordinary labels before the association is removed.

Referenced photos have no storage-implied label, so reconciliation touches only their stored ids. New label behavior
that appears to require a fourth folder-reconciliation path should first be redesigned around one of these three
boundaries.

## Item and membership mutations

Video and photo registration publish a freshly imported item only after its storage preparation succeeds. The catalog
refuses an identity already registered as a different item; re-registering the same item at its current storage is
allowed for recovery. Video registration also records whether only a permanent preview exists or full frames have
been extracted; see [import.md](import.md).

Removing an item drops the catalog record. The caller decides whether disk content must first be deleted or is
deliberately being left behind, because those are distinct user operations.

Rename callers perform and roll back the filesystem transaction; `Catalog::applyRename` carries the complete
metadata record to the new identity and paths. It refuses a destination identity already owned by another item.

`addLabel` changes stored membership only. `removeLabel` is also metadata-only unless the label names the item's
storage location; in that case storage moves to the alphabetically first remaining ordinary label. Every item must
retain at least one ordinary label when a label is removed. The same rule applies to folder-less referenced photos
even though they need no relocation.

## Registry mutations and path safety

`createLabel` creates or reuses a registry entry and ensures its direct-child backing directory.
`renameLabel` updates the backing directories and display name while preserving the id. `deleteLabel` relocates
stored items, removes additional memberships, deletes empty backing directories, and finally removes the registry
entry. Deletion is refused for `Best`, unknown ids, items that would be orphaned, or any blocked relocation;
`deleteLabelImpact` supplies the preflight summary used for confirmation.

Delete-and-relocate is intentional: a used label remains deletable when every affected item has another ordinary
label. A failed relocation leaves the registry entry intact, avoiding a model state that loading would immediately
recreate from the surviving folder.

All label paths cross one Catalog-owned validation boundary. A display name must be one portable directory component,
excluding filesystem-invalid forms and the reserved `Best` and `Photos` names. Resolved destinations must remain
direct children of the intended root and existing children must not be aliases. Create, rename, import, relocation,
and cleanup consume these verified paths rather than concatenating display names independently.

Registry mutations save `labels.json` through the shared checked atomic writer. Failed saves remain dirty and
retryable alongside metadata-store failures; see [data-model.md](data-model.md).

## Batching and change publication

Each standalone mutation writes a complete record state atomically. `Catalog::BatchScope` defers persistence and
change notification until the outermost scope ends, so a multi-item workflow serializes the store once and observers
never see its intermediate state. Nested scopes are supported.

`Catalog::ChangeBatchScope` coalesces notification without retaining a persistence writer across a long-running
event-pumping workflow. Successful visible changes publish through `Library::catalogChanged`; refused and no-op
mutations remain silent. Persistent views subscribe to `Library`, while `Catalog` stays independent of UI details.

## Loading and empty labels

Catalog loading reconstructs the in-memory item map from persisted records and re-seeds missing storage-implied
labels. Ordinary filtering, sorting, zooming, and refresh do not rebuild the model. Mutations patch the in-memory
entry directly, preserving Catalog as the live authority.

Registry seeding only adds missing labels. It never prunes a zero-item label because emptiness may be intentional or
transient, and item count is not authorization to discard identity, color, or future associations. Explicit
`deleteLabel` is the sole removal path.

## Integrity checking

`CatalogIntegrity` compares the authoritative catalog with disk only on explicit request. It detects unclaimed video
folders and owned photos, broken tracked video products, missing photo sources, and stored label ids absent from the
registry. Normal refresh never performs this scan.

The scanner reasons over the public Catalog model and filesystem without mutating either. Resolution remains in the
components that own the affected catalog or disk operation. The detailed media-state verdict grid belongs beside
`CatalogIntegrity::scan`, where it can evolve with the implementation instead of being duplicated here.
