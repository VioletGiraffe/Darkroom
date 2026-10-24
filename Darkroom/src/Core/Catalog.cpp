#include "Core/Catalog.h"
#include "Core/JsonPersistence.h"
#include "Core/MetadataStore.h"
#include "Utils.h"

#include "assert/advanced_assert.h"
#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QColor>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QRandomGenerator>
#include <QScopeGuard>
RESTORE_COMPILER_WARNINGS

#include <functional>
#include <optional>
#include <utility>

namespace
{
	constexpr QStringView kLabelsField          = u"labels";
	constexpr QStringView kFolderField          = u"folder";
	constexpr QStringView kSourcePathField      = u"sourceVideoPath";  // the historical field name - changing the stored string would orphan existing catalog records
	constexpr QStringView kSplitIntoFramesField = u"splitIntoFrames";
	constexpr QStringView kTypeField            = u"type";             // kPhotoTypeValue for photos; absent = video, so pre-photos records need no migration
	constexpr QStringView kReferencedField      = u"referenced";       // bool; photos only
	constexpr QStringView kDurationMsField      = u"durationMs";       // video source length in ms; absent = unknown (pre-existing record / photo)
	constexpr QStringView kSourceSizeField      = u"sourceSize";       // bytes; absent = unknown
	constexpr QStringView kFirstFormatNameField = u"name";             // the original-case file name; first format only
	constexpr QStringView kPhotoTypeValue       = u"photo";
	constexpr QStringView kBestLabelName        = u"Best";
	// Seeded into the label once at creation and persisted; the star glyph itself is themed separately,
	// so this is a data-model default, deliberately not a live theme colour.
	constexpr QStringView kBestLabelColor       = u"#cc8a1f";

	[[nodiscard]] bool isReservedWindowsDeviceName(const QString& name)
	{
		const QString stem = name.section('.', 0, 0).trimmed().toUpper();
		if (stem == QLatin1String("CON") || stem == QLatin1String("PRN") || stem == QLatin1String("AUX") || stem == QLatin1String("NUL"))
			return true;
		if (stem.size() != 4 || (!stem.startsWith(QLatin1String("COM")) && !stem.startsWith(QLatin1String("LPT"))))
			return false;
		const QChar suffix = stem.back();
		return (suffix >= QLatin1Char('1') && suffix <= QLatin1Char('9'))
			|| suffix == QChar(0x00b9) || suffix == QChar(0x00b2) || suffix == QChar(0x00b3);
	}

	// Backstop name validation: the result must remain a real direct child, never a symlink alias.
	[[nodiscard]] QString validatedDirectChildPath(const QString& parentFolder, const QString& childName)
	{
		const QString parent = QDir(parentFolder).absolutePath();
		const QString child = QDir(parent).absoluteFilePath(childName);
		if (pathComparisonKey(QFileInfo(child).absolutePath()) != pathComparisonKey(parent))
			return {};

		const QFileInfo childInfo(child);
		if (childInfo.isSymLink())
			return {};
		if (childInfo.exists())
		{
			// Resolve both sides so a junction in the parent path does not defeat the lexical comparison.
			const QString canonicalChild  = childInfo.canonicalFilePath();
			const QString canonicalParent = QFileInfo(parent).canonicalFilePath();
			if (canonicalChild.isEmpty()
				|| pathComparisonKey(QFileInfo(canonicalChild).absolutePath()) != pathComparisonKey(canonicalParent)
				|| QFileInfo(canonicalChild).fileName().compare(childInfo.fileName(), Qt::CaseInsensitive) != 0)
				return {};
		}
		return child;
	}

	[[nodiscard]] MediaItemId randomMediaItemId(const std::function<bool(MediaItemId)>& isTaken)
	{
		for (;;)
		{
			const auto id = static_cast<MediaItemId>(QRandomGenerator::system()->generate64());
			if (id != MediaItemId::None && !isTaken(id))
				return id;
		}
	}

	// -1 for a missing file.
	[[nodiscard]] qint64 fileSize(const QString& path)
	{
		const QFileInfo info{ path };
		return info.isFile() ? info.size() : -1;
	}
}

Catalog::Catalog(QString rootFolder, MetadataStore& metadataStore, const QJsonObject& registry)
	: _rootFolder(std::move(rootFolder)), _metadataStore(metadataStore)
{
	loadRegistry(registry);
	rebuildIndex();
}

void Catalog::notifyCatalogChanged()
{
	if (_catalogChangeBatchDepth > 0)
	{
		_catalogChangePending = true;
		return;
	}
	if (_catalogChangeHandler)
		_catalogChangeHandler();
}

void Catalog::notifyBeforeStorageMove(const QStringList& paths) const
{
	if (_storageMoveHandlers.beforeMove)
		_storageMoveHandlers.beforeMove(paths);
}

void Catalog::notifyAfterStorageMove(const QStringList& paths, const std::vector<PathMove>& completedMoves) const
{
	if (_storageMoveHandlers.afterMove)
		_storageMoveHandlers.afterMove(paths, completedMoves);
}

bool Catalog::moveStorage(const QString& from, const QString& to) const
{
	notifyBeforeStorageMove({ from });
	const bool moved = QFile::rename(from, to);
	notifyAfterStorageMove({ from }, moved ? std::vector<PathMove>{ { from, to } } : std::vector<PathMove>{});
	return moved;
}

void Catalog::finishChangeBatch()
{
	if (_catalogChangeBatchDepth <= 0)
	{
		assert_r(_catalogChangeBatchDepth > 0);
		return;
	}

	--_catalogChangeBatchDepth;
	if (_catalogChangeBatchDepth == 0 && _catalogChangePending)
	{
		_catalogChangePending = false;
		if (_catalogChangeHandler)
			_catalogChangeHandler();
	}
}

QString Catalog::registryFilePathFor(const QString& rootFolder)
{
	return rootFolder + "/labels.json";
}

QString Catalog::photosRootFolder() const
{
	return _rootFolder + "/" + PhotosDirectoryName.toString();
}

void Catalog::loadRegistry(const QJsonObject& registry)
{
	_labels.clear();
	const QJsonArray labelArray = registry.value(kLabelsField.toString()).toArray();
	for (const QJsonValue& v : labelArray)
	{
		const QJsonObject o = v.toObject();
		const LabelId id = labelIdFromUInt64(static_cast<uint64_t>(o.value("id").toInteger()));
		_nextLabelId = qMax(_nextLabelId, toUInt64(id));
		_labels.push_back(Label{ id, o.value("displayName").toString(), o.value("color").toString() });
	}
}

void Catalog::saveRegistry()
{
	_registryDirty = true;
	static_cast<void>(flushPendingRegistrySave());
}

bool Catalog::flushPendingRegistrySave(QString* error)
{
	if (error)
		error->clear();
	if (!_registryDirty)
		return true;

	QJsonArray arr;
	for (const Label& l : _labels)
	{
		QJsonObject o;
		o.insert("id", QJsonValue(static_cast<qint64>(toUInt64(l.id))));
		o.insert("displayName", l.displayName);
		o.insert("color", l.color);
		arr.append(o);
	}

	QJsonObject root;
	root.insert(kLabelsField.toString(), arr);

	const QString saveError = JsonPersistence::writeObject(registryFilePathFor(_rootFolder), root);
	if (saveError.isEmpty())
	{
		_registryDirty = false;
		_pendingRegistrySaveError.clear();
		return true;
	}

	const bool firstFailure = _pendingRegistrySaveError.isEmpty();
	_pendingRegistrySaveError = saveError;
	if (error)
		*error = saveError;
	qWarning() << "Catalog: labels registry save failed:" << saveError;
	if (firstFailure && _persistenceFailureHandler)
		_persistenceFailureHandler();
	return false;
}

void Catalog::ensureBestAndFolderLabels()
{
	bool changed = ensureBestLabelExists();

	// Seed only locations represented by model entries; empty folders and referenced photos imply no label.
	MetadataStore& store = _metadataStore;
	QSet<QString> labelNames;
	for (const MediaItemId id : store.allItemIds())
	{
		const QString folderRel = store.get(id, kFolderField).toString();
		if (folderRel.isEmpty())
			continue;
		const bool isPhoto = store.get(id, kTypeField).toString() == kPhotoTypeValue;
		labelNames.insert(isPhoto ? QFileInfo(folderRel).fileName() : storageFolderNameOf(fromStoredPath(folderRel)));
	}
	for (const QString& labelName : labelNames)
		changed = ensureFolderLabelExists(labelName) || changed;

	if (changed)
		saveRegistry();
}

bool Catalog::ensureBestLabelExists()
{
	if (labelById(BestLabelId))
		return false;
	_labels.insert(_labels.begin(), Label{ BestLabelId, kBestLabelName.toString(), kBestLabelColor.toString() });
	return true;
}

// Moderate saturation/value avoids harsh primaries and near-black swatches.
QString Catalog::randomLabelColor()
{
	auto* rng = QRandomGenerator::global();
	const int hue = rng->bounded(360);
	const int saturation = 110 + rng->bounded(80);
	const int value = 180 + rng->bounded(50);
	return QColor::fromHsv(hue, saturation, value).name();
}

bool Catalog::ensureFolderLabelExists(const QString& displayName, const QString& color)
{
	if (ordinaryLabelIdByName(displayName) != LabelId::None)
		return false;
	_labels.push_back(Label{ generateLabelId(), displayName, color.isEmpty() ? randomLabelColor() : color });
	return true;
}

const Catalog::Label* Catalog::labelById(LabelId id) const
{
	for (const Label& l : _labels)
		if (l.id == id)
			return &l;
	return nullptr;
}

Catalog::Label* Catalog::mutableLabelById(LabelId id)
{
	for (Label& l : _labels)
		if (l.id == id)
			return &l;
	return nullptr;
}

LabelId Catalog::ordinaryLabelIdByName(const QString& displayName) const
{
	// Folder-label identity is case-insensitive on every host to preserve portable behavior.
	for (const Label& l : _labels)
		if (!l.isVirtual() && l.displayName.compare(displayName, Qt::CaseInsensitive) == 0)
			return l.id;
	return LabelId::None;
}

LabelId Catalog::generateLabelId()
{
	return labelIdFromUInt64(++_nextLabelId);
}

MediaItemId Catalog::generateMediaItemId() const
{
	return randomMediaItemId([this](MediaItemId id) { return _mediaItems.contains(id); });
}

QList<LabelId> Catalog::readStoredLabelIds(MediaItemId id) const
{
	const QJsonArray arr = _metadataStore.get(id, kLabelsField).toArray();
	QList<LabelId> out;
	out.reserve(arr.size());
	for (const QJsonValue& v : arr)
		out << labelIdFromUInt64(static_cast<uint64_t>(v.toInteger()));
	return out;
}

void Catalog::writeStoredLabelIds(MediaItemId id, const QList<LabelId>& labelIds)
{
	QJsonArray arr;
	for (const LabelId labelId : labelIds)
		arr.append(QJsonValue(static_cast<qint64>(toUInt64(labelId))));
	_metadataStore.beginBatch().set(id, kLabelsField, arr);
}

QString Catalog::storageFolderNameOf(const QString& folderAbs)
{
	// A video's frame folder lives at <root>/<storageFolder>/<videoFolder>; the storage-folder name is the
	// display name of the video's storage label.
	return QFileInfo(folderAbs).dir().dirName();
}

QString Catalog::storageLabelNameOf(const Entry& e)
{
	if (e.folder.isEmpty())
		return {};
	return e.type == MediaType::Photo ? QFileInfo(e.folder).fileName() : storageFolderNameOf(e.folder);
}

LabelId Catalog::storageLabelIdOf(const Entry& e) const
{
	const QString name = storageLabelNameOf(e);
	return name.isEmpty() ? LabelId::None : ordinaryLabelIdByName(name);
}

QString Catalog::toStoredPath(const QString& absolutePath) const
{
	// Never the root itself: no item is stored there.
	return pathRelativeTo(absolutePath, _rootFolder).value_or(absolutePath);
}

QString Catalog::fromStoredPath(const QString& storedPath) const
{
	if (storedPath.isEmpty() || QDir::isAbsolutePath(storedPath))
		return storedPath;
	return _rootFolder + '/' + storedPath;
}

QList<LabelId> Catalog::computeLabelIds(MediaItemId id, const Entry& e) const
{
	QList<LabelId> labelIds;

	const LabelId storageLabelId = storageLabelIdOf(e);
	if (storageLabelId != LabelId::None)
		labelIds << storageLabelId;

	for (const LabelId stored : readStoredLabelIds(id))
		if (!labelIds.contains(stored))
			labelIds << stored;

	return labelIds;
}

void Catalog::rebuildIndex()
{
	ensureBestAndFolderLabels();

	_mediaItems.clear();
	_mediaItemBySourcePath.clear();
	_videoByFrameFolder.clear();
	MetadataStore& store = _metadataStore;
	{
		MetadataStore::Writer writer = store.beginBatch();
		for (const MediaItemId id : store.allItemIds())
		{
			const QString storedFolder = store.get(id, kFolderField).toString();
			const bool isPhoto = store.get(id, kTypeField).toString() == kPhotoTypeValue;
			if (storedFolder.isEmpty() && !isPhoto)
			{
				assert_unconditional_r(("Catalog: video record " + toString(id) + " has no frame folder").toStdString());
				continue;
			}

			const QString storedSourcePath = store.get(id, kSourcePathField).toString();
			Entry e;
			e.folder          = fromStoredPath(storedFolder);
			e.sourcePath      = fromStoredPath(storedSourcePath);
			e.sourceSize      = store.get(id, kSourceSizeField).toInteger(-1);
			e.type            = isPhoto ? MediaType::Photo : MediaType::Video;
			e.referenced      = store.get(id, kReferencedField).toBool(false);
			e.splitIntoFrames = store.get(id, kSplitIntoFramesField).toBool(true);
			e.durationMs      = store.get(id, kDurationMsField).toInteger(-1);

			// An absolute folder outside the root locates the root a moved older-format library came from: item folders lie two levels below it.
			if (!e.folder.isEmpty() && !pathRelativeTo(e.folder, _rootFolder))
			{
				const QString previousRoot = QDir::cleanPath(e.folder + "/../..");
				const auto rebased = [&](const QString& path) {
					const std::optional<QString> relative = pathRelativeTo(path, previousRoot);
					return relative ? _rootFolder + '/' + *relative : path;
				};
				e.folder     = rebased(e.folder);
				e.sourcePath = rebased(e.sourcePath);
			}
			e.labelIds = computeLabelIds(id, e);

			// Converts absolute paths under the root, which older catalogs hold, to the stored form.
			if (const QString folder = toStoredPath(e.folder); folder != storedFolder)
				writer.set(id, kFolderField, folder);
			if (const QString sourcePath = toStoredPath(e.sourcePath); sourcePath != storedSourcePath)
				writer.set(id, kSourcePathField, sourcePath);

			insertMediaItem(id, std::move(e));
		}
	}
	notifyCatalogChanged();
}

void Catalog::insertMediaItem(MediaItemId id, Entry e)
{
	assert_and_return_r(!_mediaItems.contains(id), );
	indexMediaItemPaths(id, e);
	_mediaItems.insert(id, std::move(e));
}

void Catalog::eraseMediaItem(MediaItemId id)
{
	const auto it = _mediaItems.constFind(id);
	if (it == _mediaItems.cend())
		return;
	unindexMediaItemPaths(id, *it);
	_mediaItems.erase(it);
}

void Catalog::setMediaItemPaths(MediaItemId id, Entry& e, const QString& sourcePath, const QString& folder)
{
	unindexMediaItemPaths(id, e);
	e.sourcePath = sourcePath;
	e.folder     = folder;
	indexMediaItemPaths(id, e);
}

void Catalog::indexMediaItemPaths(MediaItemId id, const Entry& e)
{
	if (!e.sourcePath.isEmpty())
		_mediaItemBySourcePath.insert(pathComparisonKey(e.sourcePath), id);
	if (e.type == MediaType::Video)
		_videoByFrameFolder.insert(pathComparisonKey(e.folder), id);
}

void Catalog::unindexMediaItemPaths(MediaItemId id, const Entry& e)
{
	// Older libraries can record one path for two items: only the item the index holds unindexes it.
	if (const QString key = pathComparisonKey(e.sourcePath); _mediaItemBySourcePath.value(key) == id)
		_mediaItemBySourcePath.remove(key);
	if (const QString key = pathComparisonKey(e.folder); e.type == MediaType::Video && _videoByFrameFolder.value(key) == id)
		_videoByFrameFolder.remove(key);
}

void Catalog::refreshMediaItemLabels(MediaItemId id)
{
	const auto it = _mediaItems.find(id);
	if (it != _mediaItems.end())
		it->labelIds = computeLabelIds(id, *it);
}

QList<LabelId> Catalog::labelsForMediaItem(MediaItemId id) const
{
	const auto it = _mediaItems.constFind(id);
	return it != _mediaItems.cend() ? it->labelIds : QList<LabelId>{};
}

QSet<MediaItemId> Catalog::mediaItemsForLabel(LabelId labelId) const
{
	QSet<MediaItemId> out;
	for (auto it = _mediaItems.cbegin(); it != _mediaItems.cend(); ++it)
		if (it->labelIds.contains(labelId))
			out.insert(it.key());
	return out;
}

bool Catalog::mediaItemHasLabel(MediaItemId id, LabelId labelId) const
{
	const auto it = _mediaItems.constFind(id);
	return it != _mediaItems.cend() && it->labelIds.contains(labelId);
}

QHash<LabelId, int> Catalog::labelMediaItemCounts() const
{
	QHash<LabelId, int> counts;
	for (auto it = _mediaItems.cbegin(); it != _mediaItems.cend(); ++it)
		for (const LabelId labelId : it->labelIds)
			++counts[labelId];
	return counts;
}

MediaItemId Catalog::mediaItemAtSourcePath(const QString& path) const
{
	return _mediaItemBySourcePath.value(pathComparisonKey(path), MediaItemId::None);
}

MediaItemId Catalog::videoAtFrameFolder(const QString& folder) const
{
	return _videoByFrameFolder.value(pathComparisonKey(folder), MediaItemId::None);
}

QString Catalog::folderForMediaItem(MediaItemId id) const
{
	return _mediaItems.value(id).folder;
}

QString Catalog::sourcePathForMediaItem(MediaItemId id) const
{
	return _mediaItems.value(id).sourcePath;
}

QString Catalog::displayName(MediaItemId id) const
{
	return QFileInfo(_mediaItems.value(id).sourcePath).completeBaseName();
}

QString Catalog::frameFolderName(const QString& baseName, const QString& sourceSuffix, qint64 sourceSize)
{
	// The separator stays when the extension is empty: "a_b" without an extension must not match "a.b"
	return baseName + "_" + sourceSuffix + "_" + QString::number(sourceSize);
}

QString Catalog::frameFolderForNewVideo(const QString& storageFolder, const QString& sourcePath) const
{
	const QFileInfo source{ sourcePath };
	const QString folder = storageFolder + "/" + frameFolderName(source.completeBaseName(), source.suffix(), source.size());
	QString candidate = folder;
	for (int number = 2; videoAtFrameFolder(candidate) != MediaItemId::None; ++number)
		candidate = folder + "_" + QString::number(number);
	return candidate;
}

QString Catalog::previewDirFor(const QString& frameFolder)
{
	return frameFolder + "/preview";
}

bool Catalog::isSplitIntoFrames(MediaItemId id) const
{
	return _mediaItems.value(id).splitIntoFrames;
}

Catalog::MediaType Catalog::mediaType(MediaItemId id) const
{
	return _mediaItems.value(id).type;
}

bool Catalog::isReferenced(MediaItemId id) const
{
	return _mediaItems.value(id).referenced;
}

qint64 Catalog::durationMsForMediaItem(MediaItemId id) const
{
	return _mediaItems.value(id).durationMs;
}

QString Catalog::anySourceDir() const
{
	for (auto it = _mediaItems.cbegin(); it != _mediaItems.cend(); ++it)
		if (it->type == MediaType::Video && !it->sourcePath.isEmpty() && QFileInfo::exists(it->sourcePath))
			return QFileInfo(it->sourcePath).absolutePath();
	return {};
}

MediaItemId Catalog::findMediaItemWithSameContent(const QString& filePath) const
{
	const qint64 size = fileSize(filePath);
	if (size < 0)
		return MediaItemId::None;

	for (auto it = _mediaItems.cbegin(); it != _mediaItems.cend(); ++it)
	{
		if (it->sourceSize == size && filesAreIdentical(filePath, it->sourcePath))
			return it.key();
	}
	return MediaItemId::None;
}

void Catalog::addLabel(MediaItemId id, LabelId labelId)
{
	if (!_mediaItems.contains(id))
	{
		qWarning() << "Catalog: cannot add label" << toUInt64(labelId) << "to unknown item" << toString(id);
		return;
	}
	if (!labelById(labelId))
	{
		qWarning() << "Catalog: cannot add unknown label" << toUInt64(labelId) << "to" << toString(id);
		return;
	}
	QList<LabelId> ids = readStoredLabelIds(id);
	if (!ids.contains(labelId))
	{
		ids << labelId;
		writeStoredLabelIds(id, ids);
		refreshMediaItemLabels(id);
		notifyCatalogChanged();
	}
}

void Catalog::removeLabel(MediaItemId id, LabelId labelId)
{
	const auto it = _mediaItems.constFind(id);
	if (it == _mediaItems.cend())
	{
		qWarning() << "Catalog: cannot remove label" << toUInt64(labelId) << "from unknown item" << toString(id);
		return;
	}

	// A storage label requires relocation; virtual labels fall through to metadata removal.
	const Label* label = labelById(labelId);
	if (label && !label->isVirtual())
	{
		if (storageLabelIdOf(*it) == labelId)
		{
			relocateFolderOffLabel(id, labelId);
			return;
		}
		// Referenced photos have no relocation path to enforce the last-ordinary-label invariant.
		if (it->folder.isEmpty() && !hasOtherOrdinaryLabel(id, labelId))
		{
			qWarning() << "Catalog: refusing to remove the last ordinary label from" << toString(id) << "- an item must always keep one";
			return;
		}
	}

	QList<LabelId> ids = readStoredLabelIds(id);
	if (ids.removeAll(labelId) > 0)
	{
		writeStoredLabelIds(id, ids);
		refreshMediaItemLabels(id);
		notifyCatalogChanged();
	}
}

bool Catalog::removeInvalidLabelReferences(MediaItemId id)
{
	if (!_mediaItems.contains(id))
		return false;

	QList<LabelId> labelIds = readStoredLabelIds(id);
	const qsizetype removed = labelIds.removeIf([this](LabelId labelId) { return !labelById(labelId); });
	if (removed == 0)
		return false;

	writeStoredLabelIds(id, labelIds);
	refreshMediaItemLabels(id);
	notifyCatalogChanged();
	return true;
}

MediaItemId Catalog::addMediaItem(const QString& sourcePath, const QString& folderAbs, bool splitIntoFrames, qint64 durationMs)
{
	// Two items sharing a source or a frame folder would each move or delete it from under the other.
	if (const MediaItemId existing = mediaItemAtSourcePath(sourcePath); existing != MediaItemId::None)
	{
		qWarning() << "Catalog: refusing to add media item" << sourcePath << "- its source is already tracked as" << toString(existing);
		return MediaItemId::None;
	}
	if (const MediaItemId existing = videoAtFrameFolder(folderAbs); existing != MediaItemId::None)
	{
		qWarning() << "Catalog: refusing to add media item" << sourcePath << "- its frame folder" << folderAbs << "belongs to" << toString(existing);
		return MediaItemId::None;
	}

	const MediaItemId id = generateMediaItemId();
	Entry e;
	e.folder          = folderAbs;
	e.sourcePath      = sourcePath;
	e.sourceSize      = fileSize(sourcePath);
	e.splitIntoFrames = splitIntoFrames;
	e.durationMs      = durationMs > 0 ? durationMs : -1;

	MetadataStore::Writer writer = _metadataStore.beginBatch();
	writer.set(id, kSourcePathField, toStoredPath(sourcePath));
	writer.set(id, kFolderField, toStoredPath(folderAbs));
	writer.set(id, kSplitIntoFramesField, splitIntoFrames);
	if (e.sourceSize >= 0)
		writer.set(id, kSourceSizeField, e.sourceSize);
	if (e.durationMs > 0)
		writer.set(id, kDurationMsField, e.durationMs);

	if (ensureFolderLabelExists(storageFolderNameOf(folderAbs)))
		saveRegistry();

	e.labelIds = computeLabelIds(id, e);
	insertMediaItem(id, std::move(e));
	notifyCatalogChanged();
	return id;
}

MediaItemId Catalog::addPhoto(const QString& sourcePath, const QString& labelDirAbs, bool referenced)
{
	if (const MediaItemId existing = mediaItemAtSourcePath(sourcePath); existing != MediaItemId::None)
	{
		qWarning() << "Catalog: refusing to add photo" << sourcePath << "- it is already tracked as" << toString(existing);
		return MediaItemId::None;
	}

	const MediaItemId id = generateMediaItemId();
	Entry e;
	e.folder     = labelDirAbs;
	e.sourcePath = sourcePath;
	e.sourceSize = fileSize(sourcePath);
	e.type       = MediaType::Photo;
	e.referenced = referenced;

	MetadataStore::Writer writer = _metadataStore.beginBatch();
	writer.set(id, kSourcePathField, toStoredPath(sourcePath));
	writer.set(id, kFolderField, toStoredPath(labelDirAbs));
	writer.set(id, kTypeField, kPhotoTypeValue.toString());
	writer.set(id, kReferencedField, referenced);
	if (e.sourceSize >= 0)
		writer.set(id, kSourceSizeField, e.sourceSize);

	if (!labelDirAbs.isEmpty() && ensureFolderLabelExists(QFileInfo(labelDirAbs).fileName()))
		saveRegistry();

	e.labelIds = computeLabelIds(id, e);
	insertMediaItem(id, std::move(e));
	notifyCatalogChanged();
	return id;
}

void Catalog::removeMediaItem(MediaItemId id)
{
	if (!_mediaItems.contains(id))
		return;
	_metadataStore.beginBatch().remove(id);
	eraseMediaItem(id);
	notifyCatalogChanged();
}

bool Catalog::updateMediaItemPaths(MediaItemId id, const QString& newSourcePath, const QString& newFolderAbs)
{
	const auto it = _mediaItems.find(id);
	if (it == _mediaItems.end())
		return false;
	if (const MediaItemId existing = mediaItemAtSourcePath(newSourcePath); existing != MediaItemId::None && existing != id)
	{
		qWarning() << "Catalog: refusing to move" << toString(id) << "to" << newSourcePath << "- already tracked as" << toString(existing);
		return false;
	}

	MetadataStore::Writer writer = _metadataStore.beginBatch();
	writer.set(id, kSourcePathField, toStoredPath(newSourcePath));
	writer.set(id, kFolderField, toStoredPath(newFolderAbs));
	if (const qint64 size = fileSize(newSourcePath); size >= 0)
	{
		writer.set(id, kSourceSizeField, size);
		it->sourceSize = size;
	}

	setMediaItemPaths(id, *it, newSourcePath, newFolderAbs);
	it->labelIds = computeLabelIds(id, *it);
	notifyCatalogChanged();
	return true;
}

bool Catalog::refreshSourceSize(MediaItemId id)
{
	const auto it = _mediaItems.find(id);
	if (it == _mediaItems.end())
		return false;
	const qint64 size = fileSize(it->sourcePath);
	if (size < 0)
		return false;
	if (size == it->sourceSize)
		return true;

	_metadataStore.beginBatch().set(id, kSourceSizeField, size);
	it->sourceSize = size;
	notifyCatalogChanged();
	return true;
}

QJsonObject Catalog::itemRecordsFromFirstFormat(const QJsonObject& document)
{
	QJsonObject records;
	for (auto it = document.begin(); it != document.end(); ++it)
	{
		// Keys are "<size>:<lowercased name>".
		QJsonObject record = it.value().toObject();
		bool sizeParsed = false;
		const qint64 size = it.key().section(':', 0, 0).toLongLong(&sizeParsed);
		const bool isPhoto = record.value(kTypeField).toString() == kPhotoTypeValue;
		// The first-format loader skipped these records: they were never items.
		if (!sizeParsed || size < 0 || record.value(kFirstFormatNameField).toString().isEmpty()
			|| (record.value(kFolderField).toString().isEmpty() && !isPhoto))
			continue;

		record.remove(kFirstFormatNameField);
		record.insert(kSourceSizeField, size);
		const MediaItemId id = randomMediaItemId([&records](MediaItemId candidate) { return records.contains(toString(candidate)); });
		records.insert(toString(id), record);
	}
	return records;
}

void Catalog::markSplitComplete(MediaItemId id)
{
	const auto it = _mediaItems.find(id);
	if (it == _mediaItems.end() || it->splitIntoFrames)
		return;

	MetadataStore::Writer writer = _metadataStore.beginBatch();
	writer.set(id, kSplitIntoFramesField, true);
	it->splitIntoFrames = true;
	notifyCatalogChanged();
}

void Catalog::markPreviewOnly(MediaItemId id)
{
	const auto it = _mediaItems.find(id);
	if (it == _mediaItems.end() || it->type != MediaType::Video || !it->splitIntoFrames)
		return;

	_metadataStore.beginBatch().set(id, kSplitIntoFramesField, false);
	it->splitIntoFrames = false;
	notifyCatalogChanged();
}

void Catalog::setDurationMs(MediaItemId id, qint64 durationMs)
{
	const auto it = _mediaItems.find(id);
	if (it == _mediaItems.end() || durationMs <= 0 || it->durationMs == durationMs)
		return;

	_metadataStore.beginBatch().set(id, kDurationMsField, durationMs);
	it->durationMs = durationMs;
	notifyCatalogChanged();
}

bool Catalog::hasOtherOrdinaryLabel(MediaItemId id, LabelId excludedLabelId) const
{
	for (const LabelId labelId : labelsForMediaItem(id))
	{
		if (labelId == excludedLabelId)
			continue;
		const Label* l = labelById(labelId);
		if (l && !l->isVirtual())
			return true;
	}
	return false;
}

void Catalog::relocateFolderOffLabel(MediaItemId id, LabelId removedLabelId)
{
	const auto entryIt = _mediaItems.find(id);
	if (entryIt == _mediaItems.end())
		return;

	// Relocation always chooses the alphabetically first remaining ordinary label.
	const Label* dest = nullptr;
	for (const LabelId labelId : entryIt->labelIds)
	{
		if (labelId == removedLabelId)
			continue;
		const Label* l = labelById(labelId);
		if (!l || l->isVirtual() || labelNameValidationError(l->displayName))
			continue;
		if (!dest || l->displayName.compare(dest->displayName, Qt::CaseInsensitive) < 0)
			dest = l;
	}

	if (!dest)
	{
		qWarning() << "Catalog: refusing to remove the last ordinary label from" << entryIt->folder << "- an item must stay in some folder";
		return;
	}

	// The nested stored-label update joins this batch; a failed move closes it without changes.
	MetadataStore::Writer writer = _metadataStore.beginBatch();

	// Videos move their frame folder; owned photos move their file and therefore source path.
	QString newFolderAbs;
	QString newSourcePath = entryIt->sourcePath;
	if (entryIt->type == MediaType::Photo)
	{
		const QString sourceDir = photoFolderForLabel(removedLabelId);
		if (sourceDir.isEmpty() || pathComparisonKey(entryIt->folder) != pathComparisonKey(sourceDir)
			|| pathComparisonKey(QFileInfo(entryIt->sourcePath).absolutePath()) != pathComparisonKey(sourceDir))
			return;
		const QString destDir = photoFolderForLabel(dest->id);
		if (destDir.isEmpty())
			return;
		const QString newFilePath = destDir + "/" + QFileInfo(entryIt->sourcePath).fileName();
		if (QFileInfo::exists(newFilePath) || mediaItemAtSourcePath(newFilePath) != MediaItemId::None)
		{
			qWarning() << "Catalog: cannot relocate" << entryIt->sourcePath << "to" << newFilePath << "- destination already exists or is recorded for another item";
			return;
		}
		assert_r(QDir{}.mkpath(destDir));
		if (!moveStorage(entryIt->sourcePath, newFilePath))
		{
			qWarning() << "Catalog: failed to relocate" << entryIt->sourcePath << "to" << newFilePath;
			return;
		}
		writer.set(id, kSourcePathField, toStoredPath(newFilePath));
		newSourcePath = newFilePath;
		newFolderAbs = destDir;
	}
	else
	{
		const QString sourceStorageFolder = storageFolderForLabel(removedLabelId);
		if (sourceStorageFolder.isEmpty())
			return;
		const QString sourceFolder = validatedDirectChildPath(sourceStorageFolder, QFileInfo(entryIt->folder).fileName());
		if (sourceFolder.isEmpty() || pathComparisonKey(entryIt->folder) != pathComparisonKey(sourceFolder))
			return;
		const QString destStorageFolder = storageFolderForLabel(dest->id);
		if (destStorageFolder.isEmpty())
			return;
		newFolderAbs = destStorageFolder + "/" + QFileInfo(entryIt->folder).fileName();
		if (QFileInfo::exists(newFolderAbs) || videoAtFrameFolder(newFolderAbs) != MediaItemId::None)
		{
			qWarning() << "Catalog: cannot relocate" << entryIt->folder << "to" << newFolderAbs << "- destination already exists or is recorded for another item";
			return;
		}
		assert_r(QDir{}.mkpath(destStorageFolder));
		if (!moveStorage(entryIt->folder, newFolderAbs))
		{
			qWarning() << "Catalog: failed to relocate" << entryIt->folder << "to" << newFolderAbs;
			return;
		}
	}

	// The destination label is now derived from storage; neither it nor the removed label belongs in extras.
	QList<LabelId> ids = readStoredLabelIds(id);
	bool changed = ids.removeAll(removedLabelId) > 0;
	changed = (ids.removeAll(dest->id) > 0) || changed;
	if (changed)
		writeStoredLabelIds(id, ids);

	writer.set(id, kFolderField, toStoredPath(newFolderAbs));
	setMediaItemPaths(id, *entryIt, newSourcePath, newFolderAbs);
	entryIt->labelIds = computeLabelIds(id, *entryIt);
	notifyCatalogChanged();
}

const char* Catalog::labelNameValidationError(const QString& displayName)
{
	if (displayName.isEmpty() || displayName.trimmed().isEmpty())
		return "The label name cannot be empty.";
	if (displayName.front().isSpace() || displayName.back().isSpace())
		return "The label name cannot begin or end with whitespace.";
	if (displayName == QLatin1String(".") || displayName == QLatin1String(".."))
		return "'.' and '..' cannot be used as label names.";
	if (displayName.endsWith(QLatin1Char('.')))
		return "The label name cannot end with a dot.";
	for (const QChar c : displayName)
		if (c.category() == QChar::Other_Control)
			return "The label name cannot contain control characters.";
	if (!invalidFilenameChar(displayName).isNull())
		return "The label name contains a character that is not allowed in file names.";
	if (displayName.compare(kBestLabelName.toString(), Qt::CaseInsensitive) == 0
		|| displayName.compare(PhotosDirectoryName.toString(), Qt::CaseInsensitive) == 0)
		return "This label name is reserved.";
	if (isReservedWindowsDeviceName(displayName))
		return "This label name is a reserved Windows device name.";
	return nullptr;
}

QString Catalog::storageFolderForLabel(LabelId labelId) const
{
	const Label* label = labelById(labelId);
	if (!label || label->isVirtual() || labelNameValidationError(label->displayName))
		return {};
	return validatedDirectChildPath(_rootFolder, label->displayName);
}

QString Catalog::photoFolderForLabel(LabelId labelId) const
{
	const Label* label = labelById(labelId);
	if (!label || label->isVirtual() || labelNameValidationError(label->displayName))
		return {};
	return validatedDirectChildPath(photosRootFolder(), label->displayName);
}

bool Catalog::renameLabel(LabelId labelId, const QString& newDisplayName, QString* error)
{
	if (error)
		error->clear();
	const auto fail = [error](const QString& message) {
		if (error)
			*error = message;
		return false;
	};

	Label* label = mutableLabelById(labelId);
	if (!label)
		return fail(QObject::tr("The label no longer exists."));
	if (label->isVirtual())
	{
		qWarning() << "Catalog: cannot rename the virtual Best label";
		return fail(QObject::tr("The Best label cannot be renamed."));
	}

	const QString& newName = newDisplayName;
	if (const char* validationError = labelNameValidationError(newName))
		return fail(QObject::tr(validationError));
	if (newName == label->displayName)
		return true;

	for (const Label& l : _labels)
		if (&l != label && l.displayName.compare(newName, Qt::CaseInsensitive) == 0)
		{
			qWarning() << "Catalog: cannot rename to" << newName << "- another label already uses that name";
			return fail(QObject::tr("A label named '%1' already exists.").arg(newName));
		}

	// Preflight both optional backing directories before renaming either, so a collision cannot split their names.
	const QString oldName = label->displayName;
	const QString oldFolder = validatedDirectChildPath(_rootFolder, oldName);
	const QString newFolder = validatedDirectChildPath(_rootFolder, newName);
	const QString oldPhotoDir = validatedDirectChildPath(photosRootFolder(), oldName);
	const QString newPhotoDir = validatedDirectChildPath(photosRootFolder(), newName);
	if (oldFolder.isEmpty() || newFolder.isEmpty() || oldPhotoDir.isEmpty() || newPhotoDir.isEmpty())
		return fail(QObject::tr("The label's storage path is not safely contained within the library."));
	const bool haveStorageFolder = QDir(oldFolder).exists();
	const bool havePhotoDir         = QDir(oldPhotoDir).exists();
	// Existing targets are expected for case-only renames on case-insensitive filesystems.
	const bool caseChangeOnly = newName.compare(oldName, Qt::CaseInsensitive) == 0;
	if (!caseChangeOnly && ((haveStorageFolder && QFileInfo::exists(newFolder)) || (havePhotoDir && QFileInfo::exists(newPhotoDir))))
	{
		qWarning() << "Catalog: cannot rename" << oldName << "to" << newName << "- a folder by that name already exists";
		return fail(QObject::tr("A folder for label '%1' already exists.").arg(newName));
	}
	QStringList movingFolders;
	if (haveStorageFolder)
		movingFolders.push_back(oldFolder);
	if (havePhotoDir)
		movingFolders.push_back(oldPhotoDir);
	std::vector<PathMove> completedMoves;
	notifyBeforeStorageMove(movingFolders);
	const auto notifyAfterMove = qScopeGuard([&] { notifyAfterStorageMove(movingFolders, completedMoves); });

	if (haveStorageFolder)
	{
		if (!QFile::rename(oldFolder, newFolder))
		{
			qWarning() << "Catalog: failed to rename folder" << oldFolder << "to" << newFolder;
			return fail(QObject::tr("Could not rename the label folder:\n%1").arg(QDir::toNativeSeparators(oldFolder)));
		}
		completedMoves.push_back({ oldFolder, newFolder });
	}
	if (havePhotoDir)
	{
		if (!QFile::rename(oldPhotoDir, newPhotoDir))
		{
			qWarning() << "Catalog: failed to rename folder" << oldPhotoDir << "to" << newPhotoDir;
			QString message = QObject::tr("Could not rename the label's photo folder:\n%1").arg(QDir::toNativeSeparators(oldPhotoDir));
			// Keep both backing directories under one name.
			if (haveStorageFolder)
			{
				if (QFile::rename(newFolder, oldFolder))
					completedMoves.clear();
				else
					message += "\n\n" + QObject::tr("Additionally, the label folder could not be renamed back and remains at:\n%1").arg(QDir::toNativeSeparators(newFolder));
			}
			return fail(message);
		}
		completedMoves.push_back({ oldPhotoDir, newPhotoDir });
	}

	// Rewrite paths before rebuilding, including case-drifted entries, or the old folder label would be reseeded.
	MetadataStore::Writer writer = _metadataStore.beginBatch();
	for (auto it = _mediaItems.cbegin(); it != _mediaItems.cend(); ++it)
	{
		if (storageLabelNameOf(*it).compare(oldName, Qt::CaseInsensitive) != 0)
			continue;
		if (it->type == MediaType::Photo ? !havePhotoDir : !haveStorageFolder)
			continue;
		if (it->type == MediaType::Photo)
		{
			writer.set(it.key(), kFolderField, toStoredPath(newPhotoDir));
			writer.set(it.key(), kSourcePathField, toStoredPath(newPhotoDir + "/" + QFileInfo(it->sourcePath).fileName()));
		}
		else
		{
			writer.set(it.key(), kFolderField, toStoredPath(newFolder + "/" + QFileInfo(it->folder).fileName()));
		}
	}

	label->displayName = newName;
	saveRegistry();
	rebuildIndex();
	return true;
}

void Catalog::setColor(LabelId labelId, const QString& color)
{
	Label* label = mutableLabelById(labelId);
	if (!label || label->color == color)
		return;
	label->color = color;
	saveRegistry();
	notifyCatalogChanged();
}

LabelId Catalog::createLabel(const QString& displayName, const QString& color, QString* error)
{
	if (error)
		error->clear();
	if (const char* validationError = labelNameValidationError(displayName))
	{
		if (error)
			*error = QObject::tr(validationError);
		return LabelId::None;
	}

	const QString folderPath = validatedDirectChildPath(_rootFolder, displayName);
	if (folderPath.isEmpty())
	{
		if (error)
			*error = QObject::tr("The label's storage path is not safely contained within the library.");
		return LabelId::None;
	}
	if (!QDir{}.mkpath(folderPath))
	{
		if (error)
			*error = QObject::tr("Could not create the label folder:\n%1").arg(QDir::toNativeSeparators(folderPath));
		return LabelId::None;
	}

	if (ensureFolderLabelExists(displayName, color))
	{
		saveRegistry();
		notifyCatalogChanged();
	}
	return ordinaryLabelIdByName(displayName);
}

Catalog::DeleteImpact Catalog::deleteLabelImpact(LabelId labelId) const
{
	DeleteImpact impact;
	const Label* label = labelById(labelId);
	if (!label || label->isVirtual())
		return impact;

	for (const MediaItemId id : mediaItemsForLabel(labelId))
	{
		const Entry entry = _mediaItems.value(id);
		if (storageLabelIdOf(entry) != labelId)
		{
			++impact.untagCount;
			// Folderless items still require one ordinary stored label.
			if (entry.folder.isEmpty() && !hasOtherOrdinaryLabel(id, labelId))
				impact.wouldOrphan = true;
			continue;
		}

		++impact.relocateCount;
		if (!hasOtherOrdinaryLabel(id, labelId))
			impact.wouldOrphan = true;
	}
	return impact;
}

bool Catalog::deleteLabel(LabelId labelId, std::vector<MediaItemId>* itemsLeftBehind)
{
	if (itemsLeftBehind)
		itemsLeftBehind->clear();

	Label* label = mutableLabelById(labelId);
	if (!label || label->isVirtual())
		return false;
	if (deleteLabelImpact(labelId).wouldOrphan)
		return false;
	BatchScope batch(*this);

	// Collect first because relocation mutates _mediaItems.
	std::vector<MediaItemId> storedHere;
	for (const MediaItemId id : mediaItemsForLabel(labelId))
		if (storageLabelIdOf(_mediaItems.value(id)) == labelId)
			storedHere.push_back(id);
	for (const MediaItemId id : storedHere)
		relocateFolderOffLabel(id, labelId);

	// Remaining carriers hold the label only as stored metadata.
	for (const MediaItemId id : mediaItemsForLabel(labelId))
	{
		QList<LabelId> ids = readStoredLabelIds(id);
		if (ids.removeAll(labelId) > 0)
		{
			writeStoredLabelIds(id, ids);
			refreshMediaItemLabels(id);
		}
	}

	// A blocked relocation still needs this registry entry; rebuilding would otherwise recreate it.
	std::vector<MediaItemId> leftBehind;
	for (const MediaItemId id : mediaItemsForLabel(labelId))
		if (storageLabelIdOf(_mediaItems.value(id)) == labelId)
			leftBehind.push_back(id);
	const bool relocatedEverything = leftBehind.empty();

	if (relocatedEverything)
	{
		const QString storageFolderPath = storageFolderForLabel(labelId);
		if (!storageFolderPath.isEmpty())
		{
			QDir storageFolder{ storageFolderPath };
			if (storageFolder.exists() && storageFolder.isEmpty())
				assert_r(storageFolder.removeRecursively());
		}
		const QString photoFolderPath = photoFolderForLabel(labelId);
		if (!photoFolderPath.isEmpty())
		{
			QDir photoDir{ photoFolderPath };
			if (photoDir.exists() && photoDir.isEmpty())
				assert_r(photoDir.removeRecursively());
		}
		std::erase_if(_labels, [&labelId](const Label& l) { return l.id == labelId; });
	}

	saveRegistry();
	rebuildIndex();
	if (itemsLeftBehind)
		*itemsLeftBehind = std::move(leftBehind);
	return relocatedEverything;
}
