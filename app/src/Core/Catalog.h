#pragma once

#include "Core/LabelId.h"
#include "Core/MediaId.h"
#include "Core/MetadataStore.h"  // BatchScope holds a MetadataStore::Writer by value

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QStringView>

#include <functional>
#include <utility>
#include <vector>

// The logical label model for the library. A label is a first-class object with a stable id (so renaming
// its display name never disturbs associations), a display name, a color, and a "virtual" flag. An item
// carries a flat set of labels: its positional folder label (derived from which storage folder its
// frames sit in) plus any extra labels assigned app-side. Extra labels are stored in MetadataStore under
// the "labels" field - a list of label *ids* - keyed by MediaId. `Best` is the one virtual label (no
// backing folder; pure membership).
//
// Catalog is THE catalog: the authoritative in-memory model of the media-item set, keyed by MediaId. For each
// item it holds its storage folder, source path, and full label-id set. An item is a video or a photo
// (MediaType): a video's folder is its frame folder (<root>/<storageFolder>/<name>); an owned photo's folder is
// the <root>/Photos/<label> dir its file sits in; a referenced photo is tracked in place - no storage folder
// at all, every label a stored extra. Every item-set / label query is answered from this in-memory model. MetadataStore is how the model is *persisted* - a dumb,
// field-granular, MediaId-keyed store shared with other features (e.g. the player's loop intervals); the
// catalog loads itself from it once at construction and writes through on each mutation. Nothing treats
// MetadataStore's records as the catalog - callers ask Catalog.
//
// Identity is minted from a file (a stat for name+size) at exactly one point: import (addMediaItem, source
// path in hand). Everywhere else an item is addressed by the MediaId it already carries; the disk is never
// walked during normal operation (only the on-demand integrity tool walks it).
//
// Owned by Library and borrowed explicitly by its consumers. GUI-thread only.
class Catalog
{
public:
	// Id of the one virtual (folderless) label.
	static constexpr LabelId BestLabelId = LabelId::Best;
	static constexpr QStringView PhotosDirectoryName = u"Photos";

	struct Label
	{
		LabelId id = LabelId::None;  // stable; distinct from the display name
		QString displayName;         // shown in the UI; an item stored in a folder of this name incidentally carries this label
		QString color;               // hex string e.g. "#378ADD"; empty = unset

		// Virtual = a filter-only label that never names a storage folder. Derived (not stored): Best is the
		// sole virtual label. A label owns nothing on disk - the folder coincidence is a per-item detail.
		[[nodiscard]] bool isVirtual() const { return id == BestLabelId; }
	};

	// Persisted per record as a "type" field; absent = video, so pre-photos catalogs need no migration.
	enum class MediaType { Video, Photo };

	// The label registry in display order. Best is pinned first.
	[[nodiscard]] const std::vector<Label>& allLabels() const { return _labels; }
	[[nodiscard]] const Label* labelById(LabelId id) const;

	// Re-syncs the in-memory model from the persisted store (and ensures every storage folder + Best has a
	// registry entry). The model is kept current by the mutations below, so this is only needed when the
	// store may have changed underneath - call it after a structural change if in doubt; it is idempotent.
	void rebuildIndex();

	// Queries - MediaId-anchored (a card carries its item's id directly).
	[[nodiscard]] QList<LabelId> labelsForMediaItem(const MediaId& id) const;        // full label-id set of the item
	[[nodiscard]] QSet<MediaId> mediaItemsForLabel(LabelId labelId) const;
	[[nodiscard]] bool mediaItemHasLabel(const MediaId& id, LabelId labelId) const;

	// The per-item record: every disk/type fact the catalog tracks for one media item. Exposed read-only (via
	// mediaItems() below) so an enumeration reads all of an item's facts off one struct instead of a per-field
	// re-query. Mutated only inside Catalog; labelIds is the derived cache that labelsForMediaItem() returns.
	struct Entry
	{
		QString       folder;         // absolute; a video's frame folder, an owned photo's <root>/Photos/<label> dir (both stored relative-to-root in JSON), empty for a referenced photo
		QString       sourcePath;     // absolute; may point at a missing/unmounted file. An owned photo's file lives inside its folder
		QList<LabelId> labelIds;      // derived storage-label id (first when known) + stored extra ids
		bool        splitIntoFrames = true;  // false = only preview frames exist yet; see isSplitIntoFrames. Video-only; photos keep the default
		qint64      durationMs = -1;         // video source length in ms; -1 = unknown (not probed yet / photo). See durationMsForMediaItem
		MediaType   type = MediaType::Video;
		bool        referenced = false;      // photos only; see isReferenced
	};

	// Enumeration / counts.
	// mediaItems() is the one enumeration primitive: a const borrow of the live model (no copy), keyed by
	// MediaId - read each item's facts straight off its Entry. Order is unspecified; QHash's ranged-for yields
	// values (Entry), so use asKeyValueRange() when the MediaId key is needed too.
	[[nodiscard]] const QHash<MediaId, Entry>& mediaItems() const { return _mediaItems; }
	[[nodiscard]] bool containsMediaItem(const MediaId& id) const { return _mediaItems.contains(id); }
	[[nodiscard]] int mediaItemCount() const { return static_cast<int>(_mediaItems.size()); }
	// labelId -> number of items carrying it, computed in one pass (for the sidebar's per-label counts).
	[[nodiscard]] QHash<LabelId, int> labelMediaItemCounts() const;

	// Per-item disk facts the catalog tracks. folderForMediaItem is absolute; both are empty for an unknown id.
	[[nodiscard]] QString folderForMediaItem(const MediaId& id) const;
	[[nodiscard]] QString sourcePathForMediaItem(const MediaId& id) const;
	// The name shown for an item (and its sort key): its source file's base name. Folder-independent, so it never
	// surfaces the hash suffix in a video's frame-folder name, and it follows a rename (which updates the stored
	// source path). Empty for an unknown id.
	[[nodiscard]] QString displayName(const MediaId& id) const;
	// The frame-folder leaf name for a video: its base name plus a base36 hash of its identity, e.g. "movie_a1b2c3d4e".
	// The hash makes the folder always valid and unique even when the base name alone would not be - empty, a reserved
	// device name, or a trailing dot/space - by guaranteeing a non-empty, alphanumeric-terminated leaf. Import and
	// rename both go through here so a video's on-disk folder is derived one way only.
	[[nodiscard]] static QString frameFolderName(const QString& baseName, const MediaId& id);
	// The preview-frames subdirectory of a video's frame folder - the single definition of the "preview"
	// subfolder name (a UI-render cache the catalog otherwise never touches). Pure path join; folder may not exist.
	[[nodiscard]] static QString previewDirFor(const QString& frameFolder);
	// False once a video was registered with only preview frames (an on-demand split is still owed); true once
	// the full frame set has been extracted. Unknown id -> true (nothing pending; photos always report true,
	// there is nothing to split). See addMediaItem.
	[[nodiscard]] bool isSplitIntoFrames(const MediaId& id) const;
	// Unknown id -> Video.
	[[nodiscard]] MediaType mediaType(const MediaId& id) const;
	// True for a photo tracked in place: Catalog label/removal operations never touch its file, and it has no
	// storage folder - all its labels are stored ids. The UI's separate physical Delete action may delete it.
	// Unknown id / video -> false.
	[[nodiscard]] bool isReferenced(const MediaId& id) const;
	// The video's source duration in ms, probed once at import (see Ffmpeg::generatePreviewFrames). -1 when
	// unknown: an unknown id, a photo, or a video imported before durations were recorded (backfilled lazily
	// on the next preview regeneration / re-export - see setDurationMs).
	[[nodiscard]] qint64 durationMsForMediaItem(const MediaId& id) const;
	// Directory of the first video whose source file is currently present, in iteration order (a sensible
	// default destination for relocating newly added source files; photos are skipped - an owned photo's
	// source lives inside the library itself). Empty if none is found.
	[[nodiscard]] QString anySourceDir() const;

	// Source path of a tracked photo whose file is byte-identical to photoPath (the same content re-encountered
	// under a different name), or empty if none. Size-gated via the MediaId's stored size, so only equal-size
	// photos are byte-compared - keeping the comparison rare. Reads file bytes, so unlike the model-only queries
	// it touches the disk (as anySourceDir does).
	[[nodiscard]] QString findPhotoBySameContent(const QString& photoPath) const;

	// Per-item membership. An invalid id (a missing/unreadable source file) can't be labeled, so these no-op
	// on one. addLabel only ever writes the stored id list. removeLabel is metadata-only too,
	// EXCEPT when labelId is the label that happens to name the item's storage location: then it relocates
	// the item on disk (a video's frame folder / an owned photo's file) to the item's alphabetically-first
	// remaining ordinary label. An item must always keep at least one ordinary label, so removing its last
	// one is refused - including for a referenced photo, whose labels are all stored ids.
	void addLabel(const MediaId& id, LabelId labelId);
	void removeLabel(const MediaId& id, LabelId labelId);

	// RAII: collapses any number of item registrations/mutations made within this scope into a single store
	// write at the end, instead of one per call. Nests freely - only the outermost scope flushes. Wrap any
	// loop that touches many items (seeding, batch import, re-export); without this, each mutation rewrites
	// the *entire* store, making an n-item loop write O(n^2) bytes overall instead of O(n). Just owns a
	// store Writer it never writes through: Catalog's mutations open their own nested Writers (per-mutation
	// atomicity), which an enclosing scope coalesces into one flush.
	class BatchScope
	{
	public:
		explicit BatchScope(Catalog& catalog) : _writer(catalog._metadataStore.beginBatch()) {}
		BatchScope(const BatchScope&) = delete;
		BatchScope& operator=(const BatchScope&) = delete;

	private:
		MetadataStore::Writer _writer;
	};

	// Media item lifecycle.
	// addMediaItem registers a freshly imported item (source path + frame folder both known); ensures the
	// folder label for its storage folder exists. splitIntoFrames records whether the folder already holds the full
	// real frame set (true) or only preview frames pending an on-demand split (false) - see isSplitIntoFrames.
	// Returns false (no-op) if this id already names an item tracked under a different folder - a name+size
	// collision with an existing video, which the caller must not paper over by leaving newly extracted frames
	// on disk untracked. Re-registering the same id at its current folder (re-export, or a later on-demand
	// split flipping splitIntoFrames to true) is not a collision and returns true, updating the entry's fields.
	// removeMediaItem only forgets the item; the caller decides whether its files were already deleted or are
	// deliberately being left behind (Remove from library). The catalog is not re-derived from a disk walk.
	// durationMs (source length in ms, from the import-time probe) is stored when > 0; pass -1 when unknown to
	// leave any previously stored duration untouched (a re-registration doesn't erase it).
	bool addMediaItem(const MediaId& id, const QString& sourcePath, const QString& folderAbs, bool splitIntoFrames, qint64 durationMs = -1);
	// addPhoto is addMediaItem's photo counterpart, same collision rule. labelDirAbs is the <root>/Photos/<label>
	// dir the (owned) photo's file sits in, empty for a referenced photo - which is registered with no labels
	// at all, so the caller must follow up with addLabel for the initial label (an owned photo derives its
	// storage label from labelDirAbs like a video does from its storage folder).
	bool addPhoto(const MediaId& id, const QString& sourcePath, const QString& labelDirAbs, bool referenced);
	void removeMediaItem(const MediaId& id);
	// Applies an in-app rename: carries the whole metadata record from oldId to newId (re-key; loop intervals
	// and labels follow the new identity), updates the stored source path + frame folder, and re-keys the
	// model entry. oldId == newId is allowed (the source file kept its name but the frame folder moved).
	// Returns false (no-op) if newId already names a different tracked item - a name+size collision, the same
	// guard as addMediaItem - so the caller can undo its on-disk renames instead of clobbering that entry.
	bool applyRename(const MediaId& oldId, const MediaId& newId, const QString& newSourcePath, const QString& newFolderAbs);
	// Backfills a video's source duration (ms) onto an already-registered item - for the paths that probe it
	// after registration (preview regeneration / re-export), unlike import which passes it to addMediaItem
	// directly. No-op on an unknown id, a non-positive duration, or one already equal to the stored value.
	void setDurationMs(const MediaId& id, qint64 durationMs);

	// Registry mutations (the label objects themselves). labelNameValidationError is the shared syntax/path-
	// component contract for every user-entered or provisional name; it returns a plain source-text alias for
	// the caller to translate, or nullptr when valid. renameLabel also
	// validates uniqueness and renames the matching on-disk folders if they exist (the storage folder and the
	// <root>/Photos/<label> dir),
	// rewrites the stored folder (and, for owned photos, source path) of every item under it, and updates the
	// registry display name - the id and every association are preserved. Returns false (with error populated)
	// for Best, an invalid/duplicate name, an unsafe path, or a failed/colliding folder rename. setColor stores
	// a hex color ("" = unset). Both persist labels.json.
	[[nodiscard]] static const char* labelNameValidationError(const QString& displayName);
	// Verified direct-child paths for an ordinary, valid label. Empty for an unknown/virtual/invalid label or
	// when an existing path aliases another location. Filesystem consumers must use these instead of composing
	// a path from Label::displayName themselves, because a legacy registry may still contain an unsafe name.
	[[nodiscard]] QString storageFolderForLabel(LabelId labelId) const;
	[[nodiscard]] QString photoFolderForLabel(LabelId labelId) const;
	bool renameLabel(LabelId labelId, const QString& newDisplayName, QString* error = nullptr);
	void setColor(LabelId labelId, const QString& color);

	// Creates a folder-backed label with this display name if none exists yet - for the user adding an empty
	// label up front, before any item lives in it (such a label can't be derived from the model, since no
	// item references its storage folder). Validates the name, creates its direct-child storage folder, and
	// persists labels.json. An explicit color is honored only when the label is actually created; an
	// already-existing label keeps its color (empty color = pick a random one). Returns the created-or-existing
	// label's id, or None with error populated on failure.
	LabelId createLabel(const QString& displayName, const QString& color = {}, QString* error = nullptr);

	// A pleasant, randomized label color ("#rrggbb") - the same generator new labels get on creation, exposed so
	// callers minting a label elsewhere (e.g. the Import dialog's staging area) can show a matching swatch up front.
	static QString randomLabelColor();

	// What deleting a label would do, for a confirmation dialog / pre-flight check (computed, no mutation).
	struct DeleteImpact
	{
		int  relocateCount = 0;      // items stored under the label (their frame folder / photo file will be moved)
		int  untagCount    = 0;      // items carrying it only as an extra tag (just untagged)
		bool wouldOrphan   = false;  // an item would lose its last ordinary label (a stored-under item with no
		                             // fallback, or a folder-less referenced photo tagged only this) -> delete refused
	};
	[[nodiscard]] DeleteImpact deleteLabelImpact(LabelId labelId) const;

	// Removes the label everywhere: relocates each item stored under it to its alphabetically-first remaining
	// ordinary label, untags any item that merely carried it, removes the (now-empty) backing storage folder
	// and the registry entry. Returns false (no-op on the registry) for Best, an unknown id, if any stored-under
	// item would be orphaned (no other ordinary label - check deleteLabelImpact().wouldOrphan first for a
	// message), or if a relocation was blocked (e.g. a name collision) so a folder still names the label.
	bool deleteLabel(LabelId labelId);

	// Marks a video as fully split after a successful extraction, or when the integrity scan finds real frames
	// behind a stale preview-only flag. No-op on an unknown id or one already marked split. Persists.
	void markSplitComplete(const MediaId& id);

	Catalog(const Catalog&) = delete;
	Catalog& operator=(const Catalog&) = delete;

private:
	friend class LibraryState;
	Catalog(QString rootFolder, MetadataStore& metadataStore, const QJsonObject& registry);

	// Registry (labels.json): load/save, and ensuring the labels the current model implies exist.
	void loadRegistry(const QJsonObject& registry);
	void saveRegistry();
	[[nodiscard]] bool flushPendingRegistrySave(QString* error = nullptr);
	[[nodiscard]] const QString& pendingRegistrySaveError() const { return _pendingRegistrySaveError; }
	void setPersistenceFailureHandler(std::function<void()> handler) { _persistenceFailureHandler = std::move(handler); }
	void ensureBestAndFolderLabels();                          // add any missing: Best + one folder-backed label per storage folder an item lives in
	bool ensureBestLabelExists();                              // returns true if it added the entry
	bool ensureFolderLabelExists(const QString& displayName, const QString& color = {});  // returns true if it added the entry
	[[nodiscard]] LabelId generateLabelId();  // ++_nextLabelId; monotonic, so never collides. Non-const.
	[[nodiscard]] LabelId ordinaryLabelIdByName(const QString& displayName) const;  // non-virtual displayName match (case-insensitive); None if none
	[[nodiscard]] QString registryPath() const;
	[[nodiscard]] QString photosRootFolder() const;

	[[nodiscard]] static QString storageFolderNameOf(const QString& folderAbs);  // the storage-folder name (a *video's* storage-label display name)
	// The label display name the entry's storage location implies: the storage-folder name for a video
	// (<root>/<storageFolder>/<frameFolder>), the label-dir name for an owned photo (<root>/Photos/<label>).
	// Empty when the entry has no storage folder (a referenced photo).
	[[nodiscard]] static QString storageLabelNameOf(const Entry& e);
	// Id of the ordinary label naming the entry's storage location; empty if none (incl. referenced photos).
	[[nodiscard]] LabelId storageLabelIdOf(const Entry& e) const;
	// True if the item carries an ordinary (non-virtual) label other than excludedLabelId - the ">= 1
	// ordinary label" invariant check shared by the relocate/untag refusal paths.
	[[nodiscard]] bool hasOtherOrdinaryLabel(const MediaId& id, LabelId excludedLabelId) const;
	[[nodiscard]] QString relativeFolder(const QString& folderAbs) const;    // strip the Library root prefix for portable storage
	[[nodiscard]] QString absoluteFolder(const QString& folderRel) const;    // re-anchor a stored relative folder under the Library root
	[[nodiscard]] QList<LabelId> computeLabelIds(const MediaId& id, const Entry& e) const;  // derived storage-label + stored extras (reads e.folder + e.type)
	void refreshMediaItemLabels(const MediaId& id);  // recompute one entry's labelIds after a mutation

	[[nodiscard]] Label* mutableLabelById(LabelId id);                       // non-const finder for registry mutations
	// Moves an item's storage (a video's frame folder / an owned photo's file) off the label that currently
	// names it, onto its alphabetically-first remaining ordinary label, and updates the model entry. Warns and
	// does nothing if no other ordinary label remains. Never reached by referenced photos (no storage label).
	void relocateFolderOffLabel(const MediaId& id, LabelId removedLabelId);

	[[nodiscard]] QList<LabelId> readStoredLabelIds(const MediaId& id) const;
	void writeStoredLabelIds(const MediaId& id, const QList<LabelId>& labelIds);

	const QString          _rootFolder;
	MetadataStore&         _metadataStore;
	std::vector<Label>    _labels;                  // registry, display order
	QHash<MediaId, Entry> _mediaItems;              // the model: MediaId -> per-item facts
	uint64_t              _nextLabelId = FirstRealLabelId - 1;  // high-water mark; generateLabelId hands out ++this. Seeded to the max loaded id on load.
	bool                  _registryDirty = false;
	QString               _pendingRegistrySaveError;
	std::function<void()> _persistenceFailureHandler;
};
