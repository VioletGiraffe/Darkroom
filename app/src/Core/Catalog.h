#pragma once

#include "Core/MediaId.h"
#include "Core/MetadataStore.h"  // BatchScope holds a MetadataStore::Writer by value

#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>

#include <vector>

// The logical label model for the library. A label is a first-class object with a stable id (so renaming
// its display name never disturbs associations), a display name, a color, and a "virtual" flag. An item
// carries a flat set of labels: its positional folder label (derived from which collection folder its
// frames sit in) plus any extra labels assigned app-side. Extra labels are stored in MetadataStore under
// the "labels" field - a list of label *ids* - keyed by MediaId. `Best` is the one virtual label (no
// backing folder; pure membership).
//
// Catalog is THE catalog: the authoritative in-memory model of the media-item set, keyed by MediaId. For each
// item it holds its storage folder, source path, and full label-id set. An item is a video or a photo
// (MediaType): a video's folder is its frame folder (<root>/<collection>/<name>); an owned photo's folder is
// the <root>/Photos/<label> dir its file sits in; a referenced photo is tracked in place - no storage folder
// at all, every label a stored extra. Every item-set / label query is answered from this in-memory model. MetadataStore is how the model is *persisted* - a dumb,
// field-granular, MediaId-keyed store shared with other features (e.g. the player's loop intervals); the
// catalog loads itself from it once at construction and writes through on each mutation. Nothing treats
// MetadataStore's records as the catalog - callers ask Catalog.
//
// Identity is minted from a file (a stat for name+size) at exactly two points: import (addMediaItem, source
// path in hand) and the one-time legacy seed (from each folder's source_info.txt). Everywhere else an item
// is addressed by the MediaId it already carries; the disk is never walked except by that seed.
//
// Single shared instance, GUI-thread only.
class Catalog
{
public:
	static Catalog& instance();

	// Reserved id of the one virtual (folderless) label. Equals the string Phase 1 already stored, so no
	// per-item data migration is needed.
	static const QString BestLabelId;

	struct Label
	{
		QString id;           // stable; never the display name (except the reserved "Best")
		QString displayName;  // shown in the UI; an item stored in a folder of this name incidentally carries this label
		QString color;        // hex string e.g. "#378ADD"; empty = unset

		// Virtual = a filter-only label that never names a storage folder. Derived (not stored): Best is the
		// sole virtual label. A label owns nothing on disk - the folder coincidence is a per-item detail.
		[[nodiscard]] bool isVirtual() const { return id == BestLabelId; }
	};

	// Persisted per record as a "type" field; absent = video, so pre-photos catalogs need no migration.
	enum class MediaType { Video, Photo };

	// The label registry in display order. Best is pinned first.
	[[nodiscard]] const std::vector<Label>& allLabels() const { return _labels; }
	[[nodiscard]] const Label* labelById(const QString& id) const;

	// Re-syncs the in-memory model from the persisted store (and ensures every collection + Best has a
	// registry entry). The model is kept current by the mutations below, so this is only needed when the
	// store may have changed underneath - call it after a structural change if in doubt; it is idempotent.
	void rebuildIndex();

	// Queries - MediaId-anchored (a card carries its item's id directly).
	[[nodiscard]] QStringList labelsForMediaItem(const MediaId& id) const;          // full label-id set of the item
	[[nodiscard]] QSet<MediaId> mediaItemsForLabel(const QString& labelId) const;
	[[nodiscard]] bool mediaItemHasLabel(const MediaId& id, const QString& labelId) const;

	// Enumeration / counts.
	[[nodiscard]] std::vector<MediaId> allMediaItems() const { return std::vector<MediaId>(_mediaItems.keyBegin(), _mediaItems.keyEnd()); }
	[[nodiscard]] bool containsMediaItem(const MediaId& id) const { return _mediaItems.contains(id); }
	[[nodiscard]] int mediaItemCount() const { return static_cast<int>(_mediaItems.size()); }
	// labelId -> number of items carrying it, computed in one pass (for the sidebar's per-label counts).
	[[nodiscard]] QHash<QString, int> labelMediaItemCounts() const;

	// Per-item disk facts the catalog tracks. folderForMediaItem is absolute; both are empty for an unknown id.
	[[nodiscard]] QString folderForMediaItem(const MediaId& id) const;
	[[nodiscard]] QString sourcePathForMediaItem(const MediaId& id) const;
	// The preview-frames subdirectory of a video's frame folder - the single definition of the "preview"
	// subfolder name (a UI-render cache the catalog otherwise never touches). Pure path join; folder may not exist.
	[[nodiscard]] static QString previewDirFor(const QString& frameFolder);
	// False once a video was registered with only preview frames (an on-demand split is still owed); true once
	// the full frame set has been extracted. Unknown id -> true (nothing pending; photos always report true,
	// there is nothing to split). See addMediaItem.
	[[nodiscard]] bool isSplitIntoFrames(const MediaId& id) const;
	// Unknown id -> Video.
	[[nodiscard]] MediaType mediaType(const MediaId& id) const;
	// True for a photo tracked in place (catalog-only): label operations never move its file, deleting it
	// never touches the file, and it has no storage folder - all its labels are stored ids. Unknown id / video -> false.
	[[nodiscard]] bool isReferenced(const MediaId& id) const;
	// Directory of the first video whose source file is currently present, in iteration order (a sensible
	// default destination for relocating newly added source files; photos are skipped - an owned photo's
	// source lives inside the library itself). Empty if none is found.
	[[nodiscard]] QString anySourceDir() const;

	// Per-item membership. A missing source file has an invalid (placeholder) id and can't be labeled, so
	// these no-op on one. addLabel only ever writes the stored id list. removeLabel is metadata-only too,
	// EXCEPT when labelId is the label that happens to name the item's storage location: then it relocates
	// the item on disk (a video's frame folder / an owned photo's file) to the item's alphabetically-first
	// remaining ordinary label. An item must always keep at least one ordinary label, so removing its last
	// one is refused - including for a referenced photo, whose labels are all stored ids.
	void addLabel(const MediaId& id, const QString& labelId);
	void removeLabel(const MediaId& id, const QString& labelId);

	// RAII: collapses any number of item registrations/mutations made within this scope into a single store
	// write at the end, instead of one per call. Nests freely - only the outermost scope flushes. Wrap any
	// loop that touches many items (seeding, batch import, re-export); without this, each mutation rewrites
	// the *entire* store, making an n-item loop write O(n^2) bytes overall instead of O(n). Just owns a
	// store Writer it never writes through: Catalog's mutations open their own nested Writers (per-mutation
	// atomicity), which an enclosing scope coalesces into one flush.
	class BatchScope
	{
	public:
		BatchScope() : _writer(MetadataStore::instance().beginBatch()) {}
		BatchScope(const BatchScope&) = delete;
		BatchScope& operator=(const BatchScope&) = delete;

	private:
		MetadataStore::Writer _writer;
	};

	// Media item lifecycle.
	// addMediaItem registers a freshly imported item (source path + frame folder both known); ensures the
	// collection's folder label exists. splitIntoFrames records whether the folder already holds the full
	// real frame set (true) or only preview frames pending an on-demand split (false) - see isSplitIntoFrames.
	// Returns false (no-op) if this id already names an item tracked under a different folder - a name+size
	// collision with an existing video, which the caller must not paper over by leaving newly extracted frames
	// on disk untracked. Re-registering the same id at its current folder (re-export, or a later on-demand
	// split flipping splitIntoFrames to true) is not a collision and returns true, updating the entry's fields.
	// removeMediaItem drops an item from the catalog entirely (delete-all), so it doesn't linger as a ghost now
	// that the catalog - not the disk walk - is the authoritative set.
	bool addMediaItem(const MediaId& id, const QString& sourcePath, const QString& folderAbs, bool splitIntoFrames);
	// addPhoto is addMediaItem's photo counterpart, same collision rule. labelDirAbs is the <root>/Photos/<label>
	// dir the (owned) photo's file sits in, empty for a referenced photo - which is registered with no labels
	// at all, so the caller must follow up with addLabel for the initial label (an owned photo derives its
	// storage label from labelDirAbs like a video does from its collection folder).
	bool addPhoto(const MediaId& id, const QString& sourcePath, const QString& labelDirAbs, bool referenced);
	void removeMediaItem(const MediaId& id);
	// Applies an in-app rename: carries the whole metadata record from oldId to newId (re-key; loop intervals
	// and labels follow the new identity), updates the stored source path + frame folder, and re-keys the
	// model entry. oldId == newId is allowed (the source file kept its name but the frame folder moved).
	// Returns false (no-op) if newId already names a different tracked item - a name+size collision, the same
	// guard as addMediaItem - so the caller can undo its on-disk renames instead of clobbering that entry.
	bool applyRename(const MediaId& oldId, const MediaId& newId, const QString& newSourcePath, const QString& newFolderAbs);

	// Registry mutations (the label objects themselves). renameLabel validates newDisplayName is unique, renames
	// the matching on-disk folders if they exist (the collection folder and the <root>/Photos/<label> dir),
	// rewrites the stored folder (and, for owned photos, source path) of every item under it, and updates the
	// registry display name - the id and every association are preserved. Returns false (no-op) for Best, an
	// empty/duplicate/reserved name, or a failed or colliding folder rename. setColor stores a hex color
	// ("" = unset). Both persist labels.json.
	bool renameLabel(const QString& labelId, const QString& newDisplayName);
	void setColor(const QString& labelId, const QString& color);

	// Creates a folder-backed label with this display name if none exists yet - for the user adding an empty
	// collection up front, before any item lives in it (such a label can't be derived from the model, since no
	// item references its collection). Persists labels.json; no-op if the name is already taken. The caller
	// creates the backing collection folder on disk.
	void createLabel(const QString& displayName);

	// What deleting a label would do, for a confirmation dialog / pre-flight check (computed, no mutation).
	struct DeleteImpact
	{
		int  relocateCount = 0;      // items stored under the label (their frame folder / photo file will be moved)
		int  untagCount    = 0;      // items carrying it only as an extra tag (just untagged)
		bool wouldOrphan   = false;  // an item would lose its last ordinary label (a stored-under item with no
		                             // fallback, or a folder-less referenced photo tagged only this) -> delete refused
	};
	[[nodiscard]] DeleteImpact deleteLabelImpact(const QString& labelId) const;

	// Removes the label everywhere: relocates each item stored under it to its alphabetically-first remaining
	// ordinary label, untags any item that merely carried it, removes the (now-empty) backing collection folder
	// and the registry entry. Returns false (no-op on the registry) for Best, an unknown id, if any stored-under
	// item would be orphaned (no other ordinary label - check deleteLabelImpact().wouldOrphan first for a
	// message), or if a relocation was blocked (e.g. a name collision) so a folder still names the label.
	bool deleteLabel(const QString& labelId);

	// --- Integrity check (manual, user-triggered; the only other place besides the legacy seed that walks disk) ---

	// A placeholder (source was missing when seeded/imported) whose recorded source path now resolves to an
	// existing file - the source has reappeared and the item can be relinked to its real identity.
	struct RelinkCandidate
	{
		MediaId placeholderId;
		QString folder;              // absolute, for display
		QString recordedSourcePath;  // where the source was last known/expected to be; now exists
	};

	// A frame folder on disk with no catalog entry: either nothing was ever recorded for it, or it has a
	// legacy source_info.txt whose recorded video collides with an id already tracked elsewhere (clashId).
	struct UntrackedFolder
	{
		QString folderPath;
		QString candidateSourcePath;   // from a legacy source_info.txt, if present and that file still exists; else empty
		MediaId clashId;               // the existing catalog id candidateSourcePath resolves to; invalid = no clash
		bool    filesIdentical = false;  // meaningful only when clashId.isValid(): byte comparison against the existing entry's source
	};

	// A tracked video whose on-disk backing is not fully intact, in any combination - the integrity grid's
	// verdicts are orthogonal (see scanIntegrity's banner), so several can hold on one entry. Carries the raw
	// disk facts; the predicates name the verdicts the dialog renders and resolves. A healthy video -> no issue.
	struct MediaIssue
	{
		MediaId id;
		QString folder;
		QString sourcePath;
		bool    sourcePresent     = false;  // source file still on disk
		bool    realFramesPresent = false;  // real frames in <folder> - the deliverable
		bool    previewPresent    = false;  // preview frames in previewDirFor(<folder>) - the card's only render source
		bool    splitComplete     = false;  // the entry's splitIntoFrames flag

		[[nodiscard]] bool isGhost() const       { return splitComplete && !realFramesPresent; }  // deliverable gone
		[[nodiscard]] bool isInvisible() const   { return !previewPresent; }                      // card can't render
		[[nodiscard]] bool isStale() const       { return !splitComplete && realFramesPresent; }  // flag disagrees with disk
		[[nodiscard]] bool sourceMissing() const { return !sourcePresent; }
		[[nodiscard]] bool healthy() const       { return sourcePresent && !isGhost() && !isInvisible() && !isStale(); }
	};

	struct IntegrityReport
	{
		std::vector<RelinkCandidate> relinkable;
		std::vector<UntrackedFolder> untracked;
		std::vector<MediaIssue>      issues;
		[[nodiscard]] bool isEmpty() const { return relinkable.empty() && untracked.empty() && issues.empty(); }
	};

	// Walks disk once (only ever on explicit user request, never part of the normal refresh path) plus the
	// in-memory model, looking for the three kinds of catalog/disk drift above. Read-only.
	[[nodiscard]] IntegrityReport scanIntegrity() const;

	// Resolves a placeholder to its real identity now that confirmedSourcePath exists. Unions the placeholder's
	// stored labels with any pre-existing (orphaned) record already under the real id - see data-model.md's
	// "Legacy seed" note on how such an orphaned record can exist. Refuses (false, qWarning) if the real id
	// already names an item tracked under a *different* folder - a separate, unrelated collision.
	bool relinkPlaceholder(const MediaId& placeholderId, const QString& confirmedSourcePath);

	// Integrity resolution for the STALE case: marks a video as fully split when its real frames exist on disk
	// but the entry was still flagged preview-only. No-op on an unknown id or one already marked split. Persists.
	void markSplitComplete(const MediaId& id);

	Catalog(const Catalog&) = delete;
	Catalog& operator=(const Catalog&) = delete;

private:
	Catalog();

	// One-time legacy migrations, run once before the model loads. migrateBestTxt folds the path-keyed best.txt
	// into the Best label (renames it to a backup afterwards). seedCatalogFromSourceInfo walks the folder tree
	// once and records each frame folder's source-video path + folder into the store; guarded by a flag in
	// labels.json (so it travels with the catalog onto another drive) and leaves the source_info.txt files in
	// place. Both resolve a folder's source video through readLegacySourceInfo (the legacy source_info.txt reader).
	void migrateBestTxt();
	void seedCatalogFromSourceInfo();
	[[nodiscard]] static QString readLegacySourceInfo(const QString& folderPath);

	// Registry (labels.json): load/save (incl. the seed flag), and ensuring the labels the current model implies exist.
	void loadRegistry();
	void saveRegistry() const;
	void ensureBestAndFolderLabels();                          // add any missing: Best + one folder-backed label per collection an item lives in
	bool ensureBestLabelExists();                              // returns true if it added the entry
	bool ensureFolderLabelExists(const QString& displayName);  // returns true if it added the entry
	[[nodiscard]] QString generateLabelId() const;
	[[nodiscard]] QString labelIdForFolderName(const QString& folderName) const;  // non-virtual displayName match; empty if none
	[[nodiscard]] static QString registryPath();

	struct Entry
	{
		QString     folder;           // absolute; a video's frame folder, an owned photo's <root>/Photos/<label> dir (both stored relative-to-root in JSON), empty for a referenced photo
		QString     sourcePath;       // absolute; may point at a missing/unmounted file. An owned photo's file lives inside its folder
		QStringList labelIds;         // derived storage-label id (first when known) + stored extra ids
		bool        splitIntoFrames = true;  // false = only preview frames exist yet; see isSplitIntoFrames. Video-only; photos keep the default
		MediaType   type = MediaType::Video;
		bool        referenced = false;      // photos only; see isReferenced
	};

	[[nodiscard]] static QString collectionNameOf(const QString& folderAbs);  // the collection-folder name (a *video's* storage-label display name)
	// The label display name the entry's storage location implies: the collection-folder name for a video
	// (<root>/<collection>/<frameFolder>), the label-dir name for an owned photo (<root>/Photos/<label>).
	// Empty when the entry has no storage folder (a referenced photo).
	[[nodiscard]] static QString storageLabelNameOf(const Entry& e);
	// Id of the ordinary label naming the entry's storage location; empty if none (incl. referenced photos).
	[[nodiscard]] QString storageLabelIdOf(const Entry& e) const;
	// True if the item carries an ordinary (non-virtual) label other than excludedLabelId - the ">= 1
	// ordinary label" invariant check shared by the relocate/untag refusal paths.
	[[nodiscard]] bool hasOtherOrdinaryLabel(const MediaId& id, const QString& excludedLabelId) const;
	[[nodiscard]] static QString relativeFolder(const QString& folderAbs);    // strip rootFolder() prefix for portable storage
	[[nodiscard]] static QString absoluteFolder(const QString& folderRel);    // re-anchor a stored relative folder under rootFolder()
	[[nodiscard]] QStringList computeLabelIds(const MediaId& id, const Entry& e) const;  // derived storage-label + stored extras (reads e.folder + e.type)
	void refreshMediaItemLabels(const MediaId& id);  // recompute one entry's labelIds after a mutation

	[[nodiscard]] Label* mutableLabelById(const QString& id);                // non-const finder for registry mutations
	// Moves an item's storage (a video's frame folder / an owned photo's file) off the label that currently
	// names it, onto its alphabetically-first remaining ordinary label, and updates the model entry. Warns and
	// does nothing if no other ordinary label remains. Never reached by referenced photos (no storage label).
	void relocateFolderOffLabel(const MediaId& id, const QString& removedLabelId);

	[[nodiscard]] static QStringList readStoredLabelIds(const MediaId& id);
	static void writeStoredLabelIds(const MediaId& id, const QStringList& labelIds);

	std::vector<Label>    _labels;                  // registry, display order
	QHash<MediaId, Entry> _mediaItems;              // the model: MediaId -> per-item facts
	bool                  _seededFromSourceInfo = false;  // legacy seed guard, persisted in labels.json
};
