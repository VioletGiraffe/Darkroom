#pragma once

#include "Core/LabelId.h"
#include "Core/MediaItemId.h"
#include "Core/MetadataStore.h"  // BatchScope holds a MetadataStore::Writer by value
#include "Utils.h"
#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QStringView>
RESTORE_COMPILER_WARNINGS

#include <functional>
#include <utility>
#include <vector>

// Authoritative GUI-thread model of the media set and stable-id label registry. A video's folder is its frame
// folder; an owned photo's is its <root>/Photos/<label> directory; a referenced photo has no storage folder.
// Each item derives one label from that location and stores any others by id in MetadataStore. Best is the sole
// folderless label. The catalog is loaded once from MetadataStore and kept current through these mutations;
// normal queries never walk the disk.
class Catalog
{
public:
	static constexpr LabelId BestLabelId = LabelId::Best;
	static constexpr QStringView PhotosDirectoryName = u"Photos";

	struct Label
	{
		LabelId id = LabelId::None;
		QString displayName;
		QString color;  // "#rrggbb"; empty = unset

		[[nodiscard]] bool isVirtual() const { return id == BestLabelId; }
	};

	// Persisted per record as a "type" field; absent = video, so pre-photos catalogs need no migration.
	enum class MediaType { Video, Photo };

	// Registry display order; Best is pinned first.
	[[nodiscard]] const std::vector<Label>& allLabels() const { return _labels; }
	[[nodiscard]] const Label* labelById(LabelId id) const;

	// Rebuilds the model from MetadataStore and ensures its storage labels and Best exist in the registry. Rewrites
	// stored absolute paths under the root, or under the previous root of a moved older-format library, as root-relative.
	void rebuildIndex();

	[[nodiscard]] QList<LabelId> labelsForMediaItem(MediaItemId id) const;
	[[nodiscard]] QSet<MediaItemId> mediaItemsForLabel(LabelId labelId) const;
	[[nodiscard]] bool mediaItemHasLabel(MediaItemId id, LabelId labelId) const;

	// Read-only through mediaItems(); mutated only by Catalog.
	struct Entry
	{
		QString       folder;         // absolute; empty for a referenced photo
		QString       sourcePath;     // absolute; may currently be unavailable
		QList<LabelId> labelIds;      // derived storage label first, then stored labels
		qint64      sourceSize = -1;  // bytes when last recorded; -1 = unknown
		bool        splitIntoFrames = true;
		qint64      durationMs = -1;
		MediaType   type = MediaType::Video;
		bool        referenced = false;
	};

	// Live, unordered model borrow. Use asKeyValueRange() when both id and entry are needed.
	[[nodiscard]] const QHash<MediaItemId, Entry>& mediaItems() const { return _mediaItems; }
	[[nodiscard]] bool containsMediaItem(MediaItemId id) const { return _mediaItems.contains(id); }
	[[nodiscard]] int mediaItemCount() const { return static_cast<int>(_mediaItems.size()); }
	[[nodiscard]] QHash<LabelId, int> labelMediaItemCounts() const;

	// By pathComparisonKey; None when no item's source is recorded at path.
	[[nodiscard]] MediaItemId mediaItemAtSourcePath(const QString& path) const;
	// By pathComparisonKey; None when no video's frame folder is folder.
	[[nodiscard]] MediaItemId videoAtFrameFolder(const QString& folder) const;
	// A tracked item whose source has the same bytes as filePath, None when there is none. Only items of the same recorded size are compared.
	[[nodiscard]] MediaItemId findMediaItemWithSameContent(const QString& filePath) const;

	// Empty for an unknown id.
	[[nodiscard]] QString folderForMediaItem(MediaItemId id) const;
	[[nodiscard]] QString sourcePathForMediaItem(MediaItemId id) const;
	// Source basename used for display and sorting; never exposes a video frame-folder suffix.
	[[nodiscard]] QString displayName(MediaItemId id) const;
	// Appends the source's extension and byte size, so even an empty, reserved, or trailing-dot basename yields a valid leaf.
	// Names folders on disk: a scheme change mismatches the folders of existing items.
	[[nodiscard]] static QString frameFolderName(const QString& baseName, const QString& sourceSuffix, qint64 sourceSize);
	// <storageFolder>/<frameFolderName> for the file at sourcePath, numbered when another video already owns that folder.
	// The returned folder may exist on disk untracked.
	[[nodiscard]] QString frameFolderForNewVideo(const QString& storageFolder, const QString& sourcePath) const;
	// Pure path join; frameFolder need not exist.
	[[nodiscard]] static QString previewDirFor(const QString& frameFolder);
	// The label registry's JSON document under rootFolder.
	[[nodiscard]] static QString registryFilePathFor(const QString& rootFolder);
	// False for a preview-only video. Unknown ids and photos return true because no split is pending.
	[[nodiscard]] bool isSplitIntoFrames(MediaItemId id) const;
	// Unknown id -> Video.
	[[nodiscard]] MediaType mediaType(MediaItemId id) const;
	// True for a photo tracked in place; catalog label/removal operations never touch its file.
	[[nodiscard]] bool isReferenced(MediaItemId id) const;
	// Video duration in ms; -1 for an unknown id, a photo, or an unprobed video.
	[[nodiscard]] qint64 durationMsForMediaItem(MediaItemId id) const;
	// Directory of the first available video source, in unspecified iteration order; empty when none exists.
	[[nodiscard]] QString anySourceDir() const;

	// Unknown ids no-op. Removing the storage label relocates an owned item's storage to its alphabetically
	// first remaining ordinary label. Removing any item's last ordinary label is refused.
	void addLabel(MediaItemId id, LabelId labelId);
	void removeLabel(MediaItemId id, LabelId labelId);
	// Drops stored label ids that no longer exist in the registry. Returns whether anything changed.
	bool removeInvalidLabelReferences(MediaItemId id);

	// Coalesces catalog-change notification without extending a persistence batch. Use around compound,
	// event-pumping workflows whose intermediate model states should not reach observers.
	class ChangeBatchScope
	{
	public:
		explicit ChangeBatchScope(Catalog& catalog) : _catalog(catalog) { ++_catalog._catalogChangeBatchDepth; }
		~ChangeBatchScope() { _catalog.finishChangeBatch(); }
		ChangeBatchScope(const ChangeBatchScope&) = delete;
		ChangeBatchScope& operator=(const ChangeBatchScope&) = delete;

	private:
		Catalog& _catalog;
	};

	// Coalesces nested catalog mutations into the outermost Writer's single flush. Use around loops because
	// each standalone mutation otherwise rewrites the complete metadata document. Change notification is
	// likewise emitted once, after the outermost Writer has flushed.
	class BatchScope
	{
	public:
		explicit BatchScope(Catalog& catalog) : _changeBatch(catalog), _writer(catalog._metadataStore.beginBatch()) {}
		BatchScope(const BatchScope&) = delete;
		BatchScope& operator=(const BatchScope&) = delete;

	private:
		// Destruction order is intentional: flush the Writer before the change batch emits.
		ChangeBatchScope _changeBatch;
		MetadataStore::Writer _writer;
	};

	// Registers a video under a newly minted id, records its source's size, and ensures its storage label exists.
	// Returns None, changing nothing, when the source path or frame folder is already tracked. durationMs <= 0 records none.
	MediaItemId addMediaItem(const QString& sourcePath, const QString& folderAbs, bool splitIntoFrames, qint64 durationMs = -1);
	// Same refusal rule for the source path. labelDirAbs is empty for a referenced photo, whose initial label must
	// be added separately.
	MediaItemId addPhoto(const QString& sourcePath, const QString& labelDirAbs, bool referenced);
	// Forgets the item without touching its files.
	void removeMediaItem(MediaItemId id);
	// Records new paths after an on-disk rename or a located source; the id and all metadata are kept. The size is
	// re-recorded when a file is at newSourcePath. Refused without mutation when another item's source is at
	// newSourcePath, so the caller can roll back disk changes.
	bool updateMediaItemPaths(MediaItemId id, const QString& newSourcePath, const QString& newFolderAbs);
	// Records the current size of the item's source file. False for an unknown id or a missing file.
	bool refreshSourceSize(MediaItemId id);
	// Backfills a video's duration; unknown ids, non-positive and unchanged values no-op.
	void setDurationMs(MediaItemId id, qint64 durationMs);

	// The current-format item records equivalent to a first-format catalog.json document: ids are minted, sizes are
	// taken from the keys, and records that were not items are dropped.
	[[nodiscard]] static QJsonObject itemRecordsFromFirstFormat(const QJsonObject& document);

	// Returns a translatable source-text error, or nullptr when displayName is a safe portable path component.
	[[nodiscard]] static const char* labelNameValidationError(const QString& displayName);
	// Verified direct-child paths. Empty for unknown, virtual, unsafe, or aliased labels; filesystem consumers
	// must not compose paths from displayName themselves.
	[[nodiscard]] QString storageFolderForLabel(LabelId labelId) const;
	[[nodiscard]] QString photoFolderForLabel(LabelId labelId) const;
	// Preserves the id and associations while renaming existing storage/photo directories and persisted item paths.
	bool renameLabel(LabelId labelId, const QString& newDisplayName, QString* error = nullptr);
	void setColor(LabelId labelId, const QString& color);

	// Creates the backing folder and registry entry, or returns the existing same-name id. color applies only
	// to a new label; empty chooses randomly. Failure returns None and populates error.
	LabelId createLabel(const QString& displayName, const QString& color = {}, QString* error = nullptr);
	// None when no non-virtual label has this name, compared case-insensitively.
	[[nodiscard]] LabelId ordinaryLabelIdByName(const QString& displayName) const;

	// The same "#rrggbb" generator used by createLabel.
	static QString randomLabelColor();

	struct DeleteImpact
	{
		int  relocateCount = 0;
		int  untagCount    = 0;
		bool wouldOrphan   = false;
	};
	[[nodiscard]] DeleteImpact deleteLabelImpact(LabelId labelId) const;

	// Relocates storage, removes extra tags, and then removes the empty backing folder and registry entry.
	// Refuses Best, unknown ids, orphaning, or incomplete relocation. Incomplete relocation keeps the label and
	// fills itemsLeftBehind with the items still stored under it; the other refusals leave it empty.
	bool deleteLabel(LabelId labelId, std::vector<MediaItemId>* itemsLeftBehind = nullptr);

	// Persists a video's completed split; unknown and already-complete ids no-op.
	void markSplitComplete(MediaItemId id);
	// Returns a video to preview-only, so viewing its frames extracts them again; unknown ids, photos and preview-only videos no-op.
	void markPreviewOnly(MediaItemId id);

	// Called around each move of item storage on disk: a label removal relocating an item, a label rename. Both optional.
	struct StorageMoveHandlers
	{
		// Receives the paths about to move.
		std::function<void(const QStringList& paths)> beforeMove;
		// Receives the same paths and the moves that completed; a failed or undone move is absent.
		std::function<void(const QStringList& paths, const std::vector<PathMove>& completedMoves)> afterMove;
	};

	Catalog(const Catalog&) = delete;
	Catalog& operator=(const Catalog&) = delete;

private:
	friend class LibraryState;
	Catalog(QString rootFolder, MetadataStore& metadataStore, const QJsonObject& registry);

	void loadRegistry(const QJsonObject& registry);
	void saveRegistry();
	[[nodiscard]] bool flushPendingRegistrySave(QString* error = nullptr);
	[[nodiscard]] const QString& pendingRegistrySaveError() const { return _pendingRegistrySaveError; }
	void setPersistenceFailureHandler(std::function<void()> handler) { _persistenceFailureHandler = std::move(handler); }
	void setCatalogChangeHandler(std::function<void()> handler) { _catalogChangeHandler = std::move(handler); }
	void setStorageMoveHandlers(StorageMoveHandlers handlers) { _storageMoveHandlers = std::move(handlers); }
	void notifyBeforeStorageMove(const QStringList& paths) const;
	void notifyAfterStorageMove(const QStringList& paths, const std::vector<PathMove>& completedMoves) const;
	// A single QFile::rename between the two notifications.
	[[nodiscard]] bool moveStorage(const QString& from, const QString& to) const;
	void notifyCatalogChanged();
	void finishChangeBatch();
	void ensureBestAndFolderLabels();
	bool ensureBestLabelExists();
	bool ensureFolderLabelExists(const QString& displayName, const QString& color = {});
	[[nodiscard]] LabelId generateLabelId();
	[[nodiscard]] MediaItemId generateMediaItemId() const;
	[[nodiscard]] QString photosRootFolder() const;

	[[nodiscard]] static QString storageFolderNameOf(const QString& folderAbs);
	// The video storage-folder or owned-photo label-directory name; empty for referenced photos.
	[[nodiscard]] static QString storageLabelNameOf(const Entry& e);
	[[nodiscard]] LabelId storageLabelIdOf(const Entry& e) const;
	[[nodiscard]] bool hasOtherOrdinaryLabel(MediaItemId id, LabelId excludedLabelId) const;
	// Persisted paths are root-relative under the library root, so the library can move; absolute elsewhere. Empty maps to empty.
	[[nodiscard]] QString toStoredPath(const QString& absolutePath) const;
	[[nodiscard]] QString fromStoredPath(const QString& storedPath) const;
	[[nodiscard]] QList<LabelId> computeLabelIds(MediaItemId id, const Entry& e) const;
	void refreshMediaItemLabels(MediaItemId id);

	// Every insertion, removal and path change goes through these, so the path indexes stay in step.
	void insertMediaItem(MediaItemId id, Entry e);
	void eraseMediaItem(MediaItemId id);
	void setMediaItemPaths(MediaItemId id, Entry& e, const QString& sourcePath, const QString& folder);
	void indexMediaItemPaths(MediaItemId id, const Entry& e);
	void unindexMediaItemPaths(MediaItemId id, const Entry& e);

	[[nodiscard]] Label* mutableLabelById(LabelId id);
	// Moves an item's storage (a video's frame folder / an owned photo's file) off the label that currently
	// names it, onto its alphabetically-first remaining ordinary label, and updates the model entry. Warns and
	// does nothing if no other ordinary label remains. Never reached by referenced photos (no storage label).
	void relocateFolderOffLabel(MediaItemId id, LabelId removedLabelId);

	[[nodiscard]] QList<LabelId> readStoredLabelIds(MediaItemId id) const;
	void writeStoredLabelIds(MediaItemId id, const QList<LabelId>& labelIds);

	const QString          _rootFolder;
	MetadataStore&         _metadataStore;
	std::vector<Label>    _labels;
	QHash<MediaItemId, Entry> _mediaItems;
	// pathComparisonKey -> item.
	QHash<QString, MediaItemId> _mediaItemBySourcePath;
	QHash<QString, MediaItemId> _videoByFrameFolder;
	uint64_t              _nextLabelId = FirstRealLabelId - 1;
	bool                  _registryDirty = false;
	QString               _pendingRegistrySaveError;
	std::function<void()> _persistenceFailureHandler;
	std::function<void()> _catalogChangeHandler;
	StorageMoveHandlers   _storageMoveHandlers;
	int                   _catalogChangeBatchDepth = 0;
	bool                  _catalogChangePending = false;
};
