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

	// Rebuilds the model from MetadataStore and ensures its storage labels and Best exist in the registry.
	void rebuildIndex();

	[[nodiscard]] QList<LabelId> labelsForMediaItem(const MediaId& id) const;
	[[nodiscard]] QSet<MediaId> mediaItemsForLabel(LabelId labelId) const;
	[[nodiscard]] bool mediaItemHasLabel(const MediaId& id, LabelId labelId) const;

	// Read-only through mediaItems(); mutated only by Catalog.
	struct Entry
	{
		QString       folder;         // absolute; empty for a referenced photo
		QString       sourcePath;     // absolute; may currently be unavailable
		QList<LabelId> labelIds;      // derived storage label first, then stored labels
		bool        splitIntoFrames = true;
		qint64      durationMs = -1;
		MediaType   type = MediaType::Video;
		bool        referenced = false;
	};

	// Live, unordered model borrow. Use asKeyValueRange() when both id and entry are needed.
	[[nodiscard]] const QHash<MediaId, Entry>& mediaItems() const { return _mediaItems; }
	[[nodiscard]] bool containsMediaItem(const MediaId& id) const { return _mediaItems.contains(id); }
	[[nodiscard]] int mediaItemCount() const { return static_cast<int>(_mediaItems.size()); }
	[[nodiscard]] QHash<LabelId, int> labelMediaItemCounts() const;

	// Empty for an unknown id.
	[[nodiscard]] QString folderForMediaItem(const MediaId& id) const;
	[[nodiscard]] QString sourcePathForMediaItem(const MediaId& id) const;
	// Source basename used for display and sorting; never exposes a video frame-folder hash suffix.
	[[nodiscard]] QString displayName(const MediaId& id) const;
	// Adds an identity hash so even an empty, reserved, or trailing-dot basename yields a valid unique leaf.
	[[nodiscard]] static QString frameFolderName(const QString& baseName, const MediaId& id);
	// Pure path join; frameFolder need not exist.
	[[nodiscard]] static QString previewDirFor(const QString& frameFolder);
	// False for a preview-only video. Unknown ids and photos return true because no split is pending.
	[[nodiscard]] bool isSplitIntoFrames(const MediaId& id) const;
	// Unknown id -> Video.
	[[nodiscard]] MediaType mediaType(const MediaId& id) const;
	// True for a photo tracked in place; catalog label/removal operations never touch its file.
	[[nodiscard]] bool isReferenced(const MediaId& id) const;
	// Video duration in ms; -1 for an unknown id, a photo, or an unprobed video.
	[[nodiscard]] qint64 durationMsForMediaItem(const MediaId& id) const;
	// Directory of the first available video source, in unspecified iteration order; empty when none exists.
	[[nodiscard]] QString anySourceDir() const;

	// Returns a byte-identical tracked photo's source path, size-gating disk comparisons; empty when none matches.
	[[nodiscard]] QString findPhotoBySameContent(const QString& photoPath) const;

	// Invalid ids no-op. Removing the storage label relocates an owned item's storage to its alphabetically
	// first remaining ordinary label. Removing any item's last ordinary label is refused.
	void addLabel(const MediaId& id, LabelId labelId);
	void removeLabel(const MediaId& id, LabelId labelId);

	// Coalesces nested catalog mutations into the outermost Writer's single flush. Use around loops because
	// each standalone mutation otherwise rewrites the complete metadata document.
	class BatchScope
	{
	public:
		explicit BatchScope(Catalog& catalog) : _writer(catalog._metadataStore.beginBatch()) {}
		BatchScope(const BatchScope&) = delete;
		BatchScope& operator=(const BatchScope&) = delete;

	private:
		MetadataStore::Writer _writer;
	};

	// Registers or updates a video and ensures its storage label exists. A matching id under another folder is
	// refused; re-registering it in place is allowed. durationMs <= 0 preserves any stored duration.
	bool addMediaItem(const MediaId& id, const QString& sourcePath, const QString& folderAbs, bool splitIntoFrames, qint64 durationMs = -1);
	// Same collision rule as addMediaItem. labelDirAbs is empty for a referenced photo, whose initial label must
	// be added separately.
	bool addPhoto(const MediaId& id, const QString& sourcePath, const QString& labelDirAbs, bool referenced);
	// Forgets the item without touching its files.
	void removeMediaItem(const MediaId& id);
	// Re-keys the complete metadata/model entry after an on-disk rename. oldId == newId is allowed; collision
	// with another tracked item is refused without mutation so the caller can roll back disk changes.
	bool applyRename(const MediaId& oldId, const MediaId& newId, const QString& newSourcePath, const QString& newFolderAbs);
	// Backfills a video's duration; invalid, non-positive, and unchanged values no-op.
	void setDurationMs(const MediaId& id, qint64 durationMs);

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
	// Refuses Best, unknown ids, orphaning, or incomplete relocation.
	bool deleteLabel(LabelId labelId);

	// Persists a video's completed split; unknown and already-complete ids no-op.
	void markSplitComplete(const MediaId& id);

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
	void ensureBestAndFolderLabels();
	bool ensureBestLabelExists();
	bool ensureFolderLabelExists(const QString& displayName, const QString& color = {});
	[[nodiscard]] LabelId generateLabelId();
	[[nodiscard]] LabelId ordinaryLabelIdByName(const QString& displayName) const;
	[[nodiscard]] QString registryPath() const;
	[[nodiscard]] QString photosRootFolder() const;

	[[nodiscard]] static QString storageFolderNameOf(const QString& folderAbs);
	// The video storage-folder or owned-photo label-directory name; empty for referenced photos.
	[[nodiscard]] static QString storageLabelNameOf(const Entry& e);
	[[nodiscard]] LabelId storageLabelIdOf(const Entry& e) const;
	[[nodiscard]] bool hasOtherOrdinaryLabel(const MediaId& id, LabelId excludedLabelId) const;
	[[nodiscard]] QString relativeFolder(const QString& folderAbs) const;
	[[nodiscard]] QString absoluteFolder(const QString& folderRel) const;
	[[nodiscard]] QList<LabelId> computeLabelIds(const MediaId& id, const Entry& e) const;
	void refreshMediaItemLabels(const MediaId& id);

	[[nodiscard]] Label* mutableLabelById(LabelId id);
	// Moves an item's storage (a video's frame folder / an owned photo's file) off the label that currently
	// names it, onto its alphabetically-first remaining ordinary label, and updates the model entry. Warns and
	// does nothing if no other ordinary label remains. Never reached by referenced photos (no storage label).
	void relocateFolderOffLabel(const MediaId& id, LabelId removedLabelId);

	[[nodiscard]] QList<LabelId> readStoredLabelIds(const MediaId& id) const;
	void writeStoredLabelIds(const MediaId& id, const QList<LabelId>& labelIds);

	const QString          _rootFolder;
	MetadataStore&         _metadataStore;
	std::vector<Label>    _labels;
	QHash<MediaId, Entry> _mediaItems;
	uint64_t              _nextLabelId = FirstRealLabelId - 1;
	bool                  _registryDirty = false;
	QString               _pendingRegistrySaveError;
	std::function<void()> _persistenceFailureHandler;
};
