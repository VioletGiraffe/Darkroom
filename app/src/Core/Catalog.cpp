#include "Core/Catalog.h"
#include "Core/MetadataStore.h"
#include "Theme/Theme.h"
#include "Utils.h"

#include <QColor>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRandomGenerator>
#include <QSaveFile>

namespace
{
	constexpr QStringView kLabelsField          = u"labels";
	constexpr QStringView kFolderField          = u"folder";
	constexpr QStringView kSourcePathField      = u"sourceVideoPath";  // the historical field name - changing the stored string would orphan existing catalog records
	constexpr QStringView kSplitIntoFramesField = u"splitIntoFrames";
	constexpr QStringView kTypeField            = u"type";             // kPhotoTypeValue for photos; absent = video, so pre-photos records need no migration
	constexpr QStringView kReferencedField      = u"referenced";       // bool; photos only
	constexpr QStringView kPhotoTypeValue       = u"photo";

	// A pleasant, randomized label color: full hue range but moderate saturation and a bright-but-not-blinding
	// value, so the swatch reads as a distinct tint rather than a harsh primary or a near-black smudge.
	QString randomLabelColor()
	{
		auto* rng = QRandomGenerator::global();
		const int hue = rng->bounded(360);
		const int saturation = 110 + rng->bounded(80);  // 110..189 of 255 - clearly tinted, never fully saturated
		const int value = 180 + rng->bounded(50);        // 180..229 of 255 - bright enough to never look dark
		return QColor::fromHsv(hue, saturation, value).name();
	}
}

const QString Catalog::BestLabelId = "Best";

Catalog& Catalog::instance()
{
	static Catalog catalog;
	return catalog;
}

Catalog::Catalog()
{
	loadRegistry();                // labels.json (+ the seed flag)
	migrateBestTxt();              // legacy: best.txt -> stored Best labels; renames best.txt afterwards
	seedCatalogFromSourceInfo();   // legacy: source_info.txt -> catalog records; one-time, guarded by the seed flag
	rebuildIndex();                // build the in-memory model from the store (and seed Best + folder labels)
}

// --- Registry (labels.json) ----------------------------------------------------------------------------

QString Catalog::registryPath()
{
	return rootFolder() + "/labels.json";
}

void Catalog::loadRegistry()
{
	_labels.clear();
	_seededFromSourceInfo = false;

	QFile file{ registryPath() };
	if (!file.open(QIODevice::ReadOnly))
		return;

	const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
	for (const QJsonValue& v : root.value(kLabelsField.toString()).toArray())
	{
		const QJsonObject o = v.toObject();
		const QString id = o.value("id").toString();
		if (id.isEmpty())
			continue;
		_labels.append(Label{ id, o.value("displayName").toString(), o.value("color").toString() });
	}
	_seededFromSourceInfo = root.value("seededFromSourceInfo").toBool();
}

void Catalog::saveRegistry() const
{
	QJsonArray arr;
	for (const Label& l : _labels)
	{
		QJsonObject o;
		o.insert("id", l.id);
		o.insert("displayName", l.displayName);
		o.insert("color", l.color);
		arr.append(o);
	}

	QJsonObject root;
	root.insert(kLabelsField.toString(), arr);
	root.insert("seededFromSourceInfo", _seededFromSourceInfo);

	QDir{}.mkpath(rootFolder());
	QSaveFile file{ registryPath() };
	if (!file.open(QIODevice::WriteOnly))
		return;
	file.write(QJsonDocument{ root }.toJson(QJsonDocument::Indented));
	file.commit();
}

void Catalog::ensureBestAndFolderLabels()
{
	bool changed = ensureBestLabelExists();

	// Folder labels come from where items actually live, not from a disk walk - so an empty collection folder
	// no longer yields a label. A video's label is the collection segment of its folder; an owned photo's is
	// its label-dir name (the last segment of Photos/<label>). Referenced photos have no folder and seed
	// nothing - all their labels are stored ids that must already exist in the registry.
	MetadataStore& store = MetadataStore::instance();
	QSet<QString> labelNames;
	for (const MediaId& id : store.allMediaIds())
	{
		const QString folderRel = store.get(id, kFolderField).toString();
		if (folderRel.isEmpty())
			continue;
		const bool isPhoto = store.get(id, kTypeField).toString() == kPhotoTypeValue;
		labelNames.insert(isPhoto ? QFileInfo(folderRel).fileName() : collectionNameOf(absoluteFolder(folderRel)));
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
	_labels.prepend(Label{ BestLabelId, "Best", Theme::StarActive });  // pin Best first (virtual is derived from the id); gold, matching the star
	return true;
}

bool Catalog::ensureFolderLabelExists(const QString& displayName)
{
	if (!labelIdForFolderName(displayName).isEmpty())
		return false;  // a folder-backed label with this name already exists
	_labels.append(Label{ generateLabelId(), displayName, randomLabelColor() });
	return true;
}

const Catalog::Label* Catalog::labelById(const QString& id) const
{
	for (const Label& l : _labels)
		if (l.id == id)
			return &l;
	return nullptr;
}

Catalog::Label* Catalog::mutableLabelById(const QString& id)
{
	for (Label& l : _labels)
		if (l.id == id)
			return &l;
	return nullptr;
}

QString Catalog::labelIdForFolderName(const QString& folderName) const
{
	// Only ordinary (non-virtual) labels can name a storage folder; the virtual Best never does.
	// Case-insensitive: the (Windows) filesystem is, so a collection folder differing from the label's
	// display name only in case is the same folder - matching exactly would mint a duplicate, case-variant
	// label (which renameLabel's own case-insensitive uniqueness check then refuses to ever merge back).
	for (const Label& l : _labels)
		if (!l.isVirtual() && l.displayName.compare(folderName, Qt::CaseInsensitive) == 0)
			return l.id;
	return {};
}

QString Catalog::generateLabelId() const
{
	QString id;
	do
		id = "lbl_" + QString::number(QRandomGenerator::global()->generate(), 16);
	while (id == BestLabelId || labelById(id) != nullptr);
	return id;
}

// --- Per-item stored label ids (MetadataStore "labels" field) ------------------------------------------

QStringList Catalog::readStoredLabelIds(const MediaId& id)
{
	const QJsonArray arr = MetadataStore::instance().get(id, kLabelsField).toArray();
	QStringList out;
	out.reserve(arr.size());
	for (const QJsonValue& v : arr)
		out << v.toString();
	return out;
}

void Catalog::writeStoredLabelIds(const MediaId& id, const QStringList& labelIds)
{
	QJsonArray arr;
	for (const QString& labelId : labelIds)
		arr.append(labelId);
	MetadataStore::instance().beginBatch().set(id, kLabelsField, arr);  // single write; the temporary Writer flushes right here (or joins an enclosing batch)
}

// --- Folder helpers ------------------------------------------------------------------------------------

QString Catalog::collectionNameOf(const QString& folderAbs)
{
	// A video's frame folder lives at <root>/<collection>/<videoFolder>; the collection-folder name is the
	// display name of the video's storage label.
	return QFileInfo(folderAbs).dir().dirName();
}

QString Catalog::storageLabelNameOf(const Entry& e)
{
	if (e.folder.isEmpty())
		return {};  // a referenced photo - no storage folder, no derived label
	// An owned photo's folder is the label dir itself (<root>/Photos/<label>); a video's label is the
	// collection segment above its frame folder.
	return e.type == MediaType::Photo ? QFileInfo(e.folder).fileName() : collectionNameOf(e.folder);
}

QString Catalog::storageLabelIdOf(const Entry& e) const
{
	const QString name = storageLabelNameOf(e);
	return name.isEmpty() ? QString{} : labelIdForFolderName(name);
}

QString Catalog::relativeFolder(const QString& folderAbs)
{
	const QString root = rootFolder();
	if (folderAbs.startsWith(root + '/'))
		return folderAbs.mid(root.length() + 1);
	return folderAbs;  // not under root (shouldn't happen) - store as-is rather than mangle it
}

QString Catalog::absoluteFolder(const QString& folderRel)
{
	if (QDir::isAbsolutePath(folderRel))
		return folderRel;  // tolerate a legacy absolute value
	return rootFolder() + '/' + folderRel;
}

// --- The model -----------------------------------------------------------------------------------------

QStringList Catalog::computeLabelIds(const MediaId& id, const Entry& e) const
{
	QStringList labelIds;

	const QString storageLabelId = storageLabelIdOf(e);
	if (!storageLabelId.isEmpty())
		labelIds << storageLabelId;

	for (const QString& stored : readStoredLabelIds(id))
		if (!labelIds.contains(stored))
			labelIds << stored;

	return labelIds;
}

void Catalog::rebuildIndex()
{
	ensureBestAndFolderLabels();  // every collection an item lives in + Best has a registry label before labels resolve

	_mediaItems.clear();
	MetadataStore& store = MetadataStore::instance();
	for (const MediaId& id : store.allMediaIds())
	{
		const QString folderRel = store.get(id, kFolderField).toString();
		const bool isPhoto = store.get(id, kTypeField).toString() == kPhotoTypeValue;
		if (folderRel.isEmpty() && !isPhoto)
			continue;  // a folder-less video record isn't a catalog item (e.g. a legacy orphan carrying only
			           // labels); a folder-less *photo* is a referenced photo - a real item tracked in place

		Entry e;
		e.folder          = folderRel.isEmpty() ? QString{} : absoluteFolder(folderRel);
		e.sourcePath      = store.get(id, kSourcePathField).toString();
		e.type            = isPhoto ? MediaType::Photo : MediaType::Video;
		e.referenced      = store.get(id, kReferencedField).toBool(false);
		e.splitIntoFrames = store.get(id, kSplitIntoFramesField).toBool(true);  // absent -> pre-existing, already split
		e.labelIds        = computeLabelIds(id, e);
		_mediaItems.insert(id, e);
	}
}

void Catalog::refreshMediaItemLabels(const MediaId& id)
{
	const auto it = _mediaItems.find(id);
	if (it != _mediaItems.end())
		it->labelIds = computeLabelIds(id, *it);
}

// --- Queries (MediaId-anchored) ------------------------------------------------------------------------

QStringList Catalog::labelsForMediaItem(const MediaId& id) const
{
	const auto it = _mediaItems.constFind(id);
	return it != _mediaItems.cend() ? it->labelIds : QStringList{};
}

QSet<MediaId> Catalog::mediaItemsForLabel(const QString& labelId) const
{
	QSet<MediaId> out;
	for (auto it = _mediaItems.cbegin(); it != _mediaItems.cend(); ++it)
		if (it->labelIds.contains(labelId))
			out.insert(it.key());
	return out;
}

bool Catalog::mediaItemHasLabel(const MediaId& id, const QString& labelId) const
{
	const auto it = _mediaItems.constFind(id);
	return it != _mediaItems.cend() && it->labelIds.contains(labelId);
}

QHash<QString, int> Catalog::labelMediaItemCounts() const
{
	QHash<QString, int> counts;
	for (auto it = _mediaItems.cbegin(); it != _mediaItems.cend(); ++it)
		for (const QString& labelId : it->labelIds)
			++counts[labelId];
	return counts;
}

QString Catalog::folderForMediaItem(const MediaId& id) const
{
	return _mediaItems.value(id).folder;
}

QString Catalog::sourcePathForMediaItem(const MediaId& id) const
{
	return _mediaItems.value(id).sourcePath;
}

bool Catalog::isSplitIntoFrames(const MediaId& id) const
{
	return _mediaItems.value(id).splitIntoFrames;
}

Catalog::MediaType Catalog::mediaType(const MediaId& id) const
{
	return _mediaItems.value(id).type;
}

bool Catalog::isReferenced(const MediaId& id) const
{
	return _mediaItems.value(id).referenced;
}

QString Catalog::anySourceDir() const
{
	for (auto it = _mediaItems.cbegin(); it != _mediaItems.cend(); ++it)
		if (it->type == MediaType::Video && !it->sourcePath.isEmpty() && QFileInfo::exists(it->sourcePath))
			return QFileInfo(it->sourcePath).absolutePath();
	return {};
}

// --- Per-item membership (MediaId-anchored) -----------------------------------------------------------

void Catalog::addLabel(const MediaId& id, const QString& labelId)
{
	if (!id.isValid())
	{
		qWarning() << "Catalog: cannot add label" << labelId << "- invalid media id (source file missing?)";
		return;
	}
	QStringList ids = readStoredLabelIds(id);
	if (!ids.contains(labelId))
	{
		ids << labelId;
		writeStoredLabelIds(id, ids);
		refreshMediaItemLabels(id);
	}
}

void Catalog::removeLabel(const MediaId& id, const QString& labelId)
{
	if (!id.isValid())
	{
		qWarning() << "Catalog: cannot remove label" << labelId << "- invalid media id (source file missing?)";
		return;
	}

	// Removing the ordinary label that names this item's storage location is a disk relocation, not a metadata
	// edit. A virtual label (Best) never names one, so it always falls through to the stored-id strip below.
	const Label* label = labelById(labelId);
	if (label && !label->isVirtual())
	{
		const auto it = _mediaItems.constFind(id);
		if (it != _mediaItems.cend())
		{
			if (storageLabelIdOf(*it) == labelId)
			{
				relocateFolderOffLabel(id, labelId);
				return;
			}
			// A folder-less item (a referenced photo) stores all its labels, so no relocate path protects it:
			// enforce the ">= 1 ordinary label" invariant here, same as relocateFolderOffLabel does for
			// stored-under items.
			if (it->folder.isEmpty() && !hasOtherOrdinaryLabel(id, labelId))
			{
				qWarning() << "Catalog: refusing to remove the last ordinary label from" << id.key() << "- an item must always keep one";
				return;
			}
		}
	}

	QStringList ids = readStoredLabelIds(id);
	if (ids.removeAll(labelId) > 0)
	{
		writeStoredLabelIds(id, ids);
		refreshMediaItemLabels(id);
	}
}

// --- Media item lifecycle ------------------------------------------------------------------------------

bool Catalog::addMediaItem(const MediaId& id, const QString& sourcePath, const QString& folderAbs, bool splitIntoFrames)
{
	if (!id.isValid())
	{
		qWarning() << "Catalog: cannot add media item with an invalid id, source" << sourcePath;
		return false;
	}

	// A name+size collision with an item already tracked under a *different* folder is a genuine duplicate
	// (or two distinct files that happen to collide) - refuse rather than silently overwriting the existing
	// entry's folder, which would orphan that folder and its labels. Re-registering the same id at the same
	// folder (re-export) is not a collision and falls through normally.
	const auto existing = _mediaItems.constFind(id);
	if (existing != _mediaItems.constEnd() && existing->folder != folderAbs)
	{
		qWarning() << "Catalog: refusing to add media item, id" << id.key() << "is already tracked at" << existing->folder
		           << "- collides with" << sourcePath;
		return false;
	}

	MetadataStore::Writer writer = MetadataStore::instance().beginBatch();  // one atomic disk write for the whole record update
	writer.set(id, kSourcePathField, sourcePath);
	writer.set(id, kFolderField, relativeFolder(folderAbs));
	writer.set(id, kSplitIntoFramesField, splitIntoFrames);

	if (ensureFolderLabelExists(collectionNameOf(folderAbs)))
		saveRegistry();

	Entry e;
	e.folder          = folderAbs;
	e.sourcePath      = sourcePath;
	e.splitIntoFrames = splitIntoFrames;
	e.labelIds        = computeLabelIds(id, e);
	_mediaItems.insert(id, e);
	return true;
}

bool Catalog::addPhoto(const MediaId& id, const QString& sourcePath, const QString& labelDirAbs, bool referenced)
{
	if (!id.isValid())
	{
		qWarning() << "Catalog: cannot add photo with an invalid id, source" << sourcePath;
		return false;
	}

	// Same collision rule as addMediaItem: an id already tracked under a different folder is a genuine
	// name+size duplicate - refuse rather than orphan the existing entry's storage. (The import path resolves
	// owned-import collisions by auto-renaming the incoming file before ever calling this.)
	const auto existing = _mediaItems.constFind(id);
	if (existing != _mediaItems.constEnd() && existing->folder != labelDirAbs)
	{
		qWarning() << "Catalog: refusing to add photo, id" << id.key() << "is already tracked at" << existing->folder
		           << "- collides with" << sourcePath;
		return false;
	}

	MetadataStore::Writer writer = MetadataStore::instance().beginBatch();  // one atomic disk write for the whole record update
	writer.set(id, kSourcePathField, sourcePath);
	writer.set(id, kFolderField, relativeFolder(labelDirAbs));
	writer.set(id, kTypeField, kPhotoTypeValue.toString());
	writer.set(id, kReferencedField, referenced);  // always written, so a re-import under the other mode can't leave a stale flag

	if (!labelDirAbs.isEmpty() && ensureFolderLabelExists(QFileInfo(labelDirAbs).fileName()))
		saveRegistry();

	Entry e;
	e.folder     = labelDirAbs;
	e.sourcePath = sourcePath;
	e.type       = MediaType::Photo;
	e.referenced = referenced;
	e.labelIds   = computeLabelIds(id, e);
	_mediaItems.insert(id, e);
	return true;
}

void Catalog::removeMediaItem(const MediaId& id)
{
	MetadataStore::instance().beginBatch().remove(id);
	_mediaItems.remove(id);
}

bool Catalog::applyRename(const MediaId& oldId, const MediaId& newId, const QString& newSourcePath, const QString& newFolderAbs)
{
	// A rename landing on an id already tracked as a *different* item (the new name + size matching one
	// elsewhere in the library) would silently overwrite that entry and orphan its folder - refuse instead,
	// mirroring addMediaItem. oldId == newId (a folder-only rename) is the entry itself, never a collision.
	if (oldId != newId && _mediaItems.contains(newId))
	{
		qWarning() << "Catalog: refusing to rename to id" << newId.key() << "- already tracked at" << _mediaItems.value(newId).folder;
		return false;
	}

	MetadataStore::Writer writer = MetadataStore::instance().beginBatch();  // one atomic disk write for the re-key + field updates
	writer.rekey(oldId, newId);  // no-op when ids are equal; carries loop intervals + labels to the new identity
	writer.set(newId, kSourcePathField, newSourcePath);
	writer.set(newId, kFolderField, relativeFolder(newFolderAbs));

	Entry e = _mediaItems.value(oldId);  // carries splitIntoFrames/type/referenced across the re-key
	_mediaItems.remove(oldId);
	e.folder     = newFolderAbs;
	e.sourcePath = newSourcePath;
	e.labelIds   = computeLabelIds(newId, e);
	_mediaItems.insert(newId, e);
	return true;
}

// --- Integrity check -------------------------------------------------------------------------------------

Catalog::IntegrityReport Catalog::scanIntegrity() const
{
	IntegrityReport report;

	for (auto it = _mediaItems.cbegin(); it != _mediaItems.cend(); ++it)
	{
		// The v1 integrity scan is video-only (photo support is backlog): every check below reasons about
		// frame folders, and a referenced photo's empty folder would misread as a ghost.
		if (it->type == MediaType::Photo)
			continue;

		// Relinkable: a placeholder (missing source at seed/import time) whose recorded path now exists.
		if (!it.key().isValid())
		{
			if (!it->sourcePath.isEmpty() && QFileInfo::exists(it->sourcePath))
				report.relinkable.push_back({ it.key(), it->folder, it->sourcePath });
			continue;
		}

		// Ghost: a tracked, fully-split video whose frame folder has no images left. A video still awaiting its
		// on-demand split legitimately has no real frames yet (only preview/ ones) - see isSplitIntoFrames.
		if (it->splitIntoFrames && QDir(it->folder).entryList(IMAGE_FILE_FILTERS, QDir::Files).isEmpty())
		{
			GhostEntry ghost;
			ghost.id             = it.key();
			ghost.folder         = it->folder;
			ghost.sourcePath = it->sourcePath;
			ghost.sourcePresent  = !it->sourcePath.isEmpty() && QFileInfo::exists(it->sourcePath);
			report.ghosts.push_back(ghost);
		}
	}

	// Untracked: every non-empty frame folder on disk that isn't any entry's folder.
	QSet<QString> knownFolders;
	for (auto it = _mediaItems.cbegin(); it != _mediaItems.cend(); ++it)
		knownFolders.insert(it->folder);

	forEachFolder(rootFolder(), [&](const QString& collection, const QString& folderPath) {
		if (collection.compare(PHOTOS_DIR_NAME, Qt::CaseInsensitive) == 0)
			return;  // <root>/Photos holds owned photo files (one dir per label), not video frame folders
		if (knownFolders.contains(folderPath))
			return;
		if (QDir(folderPath).entryList(IMAGE_FILE_FILTERS, QDir::Files).isEmpty())
			return;  // an empty/junk dir, not a video the catalog is missing

		UntrackedFolder u;
		u.folderPath = folderPath;
		const QString recorded = readLegacySourceInfo(folderPath);
		if (!recorded.isEmpty() && QFileInfo::exists(recorded))
		{
			u.candidateSourcePath = recorded;
			const MediaId candidateId = MediaId::fromFile(recorded);
			const auto existing = _mediaItems.constFind(candidateId);
			if (existing != _mediaItems.cend())
			{
				u.clashId = candidateId;
				u.filesIdentical = filesAreIdentical(recorded, existing->sourcePath);
			}
		}
		report.untracked.push_back(u);
	});

	return report;
}

bool Catalog::relinkPlaceholder(const MediaId& placeholderId, const QString& confirmedSourcePath)
{
	if (placeholderId.isValid())
	{
		qWarning() << "Catalog: relinkPlaceholder called with a non-placeholder id";
		return false;
	}
	const MediaId realId = MediaId::fromFile(confirmedSourcePath);
	if (!realId.isValid())
	{
		qWarning() << "Catalog: cannot relink - no file at" << confirmedSourcePath;
		return false;
	}

	const auto placeholderEntry = _mediaItems.constFind(placeholderId);
	if (placeholderEntry == _mediaItems.cend())
	{
		qWarning() << "Catalog: relinkPlaceholder - unknown placeholder id" << placeholderId.key();
		return false;
	}
	const QString folderAbs = placeholderEntry->folder;

	// A name+size collision with an item already tracked under a *different* folder is a separate, unrelated
	// problem (not the placeholder being relinked) - refuse rather than clobbering that entry, mirroring addMediaItem.
	const auto existingReal = _mediaItems.constFind(realId);
	if (existingReal != _mediaItems.cend() && existingReal->folder != folderAbs)
	{
		qWarning() << "Catalog: refusing to relink" << folderAbs << "- id" << realId.key()
		           << "is already tracked at" << existingReal->folder;
		return false;
	}

	// Union the placeholder's stored labels with any pre-existing (orphaned) record already under the real id:
	// a video whose source was missing at seed time but had labels/Best from before the source went missing is
	// stored under its real id with no folder (skipped by rebuildIndex) - see data-model.md's "Legacy seed" note.
	QStringList labels = readStoredLabelIds(realId);
	for (const QString& placeholderLabel : readStoredLabelIds(placeholderId))
		if (!labels.contains(placeholderLabel))
			labels << placeholderLabel;

	const bool splitIntoFrames = placeholderEntry->splitIntoFrames;

	// One atomic disk write for the remove + record rebuild (writeStoredLabelIds' own Writer nests into this one).
	MetadataStore::Writer writer = MetadataStore::instance().beginBatch();
	writer.remove(placeholderId);
	writer.set(realId, kSourcePathField, confirmedSourcePath);
	writer.set(realId, kFolderField, relativeFolder(folderAbs));
	writer.set(realId, kSplitIntoFramesField, splitIntoFrames);
	writeStoredLabelIds(realId, labels);

	_mediaItems.remove(placeholderId);
	Entry e;
	e.folder          = folderAbs;
	e.sourcePath      = confirmedSourcePath;
	e.splitIntoFrames = splitIntoFrames;
	e.labelIds        = computeLabelIds(realId, e);
	_mediaItems.insert(realId, e);
	return true;
}

bool Catalog::hasOtherOrdinaryLabel(const MediaId& id, const QString& excludedLabelId) const
{
	for (const QString& labelId : labelsForMediaItem(id))
	{
		if (labelId == excludedLabelId)
			continue;
		const Label* l = labelById(labelId);
		if (l && !l->isVirtual())
			return true;
	}
	return false;
}

void Catalog::relocateFolderOffLabel(const MediaId& id, const QString& removedLabelId)
{
	const auto entryIt = _mediaItems.find(id);
	if (entryIt == _mediaItems.end())
		return;

	// Destination = the alphabetically-first of the item's *remaining* ordinary (non-virtual) labels.
	const Label* dest = nullptr;
	for (const QString& labelId : entryIt->labelIds)
	{
		if (labelId == removedLabelId)
			continue;
		const Label* l = labelById(labelId);
		if (!l || l->isVirtual())
			continue;
		if (!dest || l->displayName.compare(dest->displayName, Qt::CaseInsensitive) < 0)
			dest = l;
	}

	if (!dest)
	{
		qWarning() << "Catalog: refusing to remove the last ordinary label from" << entryIt->folder << "- an item must stay in some folder";
		return;
	}

	// One atomic disk write for everything below (the stored-id strip's own Writer nests into this one);
	// opened before the branch so the photo path can write through it too. An early return on a failed
	// move just closes it with nothing written.
	MetadataStore::Writer writer = MetadataStore::instance().beginBatch();

	// The disk move: a video relocates its whole frame folder into the destination's collection folder; an
	// owned photo relocates its file into the destination's label dir under <root>/Photos, and its folder
	// field is that dir. (Referenced photos never reach here - they have no storage label to remove.)
	QString newFolderAbs;
	if (entryIt->type == MediaType::Photo)
	{
		const QString destDir     = photosRootFolder() + "/" + dest->displayName;
		const QString newFilePath = destDir + "/" + QFileInfo(entryIt->sourcePath).fileName();
		if (QFileInfo::exists(newFilePath))
		{
			qWarning() << "Catalog: cannot relocate" << entryIt->sourcePath << "to" << newFilePath << "- destination already exists";
			return;
		}
		QDir{}.mkpath(destDir);  // the destination label may not have a photo dir yet - created lazily
		if (!QFile::rename(entryIt->sourcePath, newFilePath))
		{
			qWarning() << "Catalog: failed to relocate" << entryIt->sourcePath << "to" << newFilePath;
			return;
		}
		// Unlike a video's, an owned photo's source path moves along with its storage - persist it too.
		writer.set(id, kSourcePathField, newFilePath);
		entryIt->sourcePath = newFilePath;
		newFolderAbs = destDir;
	}
	else
	{
		const QString destCollection = rootFolder() + "/" + dest->displayName;
		newFolderAbs = destCollection + "/" + QFileInfo(entryIt->folder).fileName();
		if (QFileInfo::exists(newFolderAbs))
		{
			qWarning() << "Catalog: cannot relocate" << entryIt->folder << "to" << newFolderAbs << "- destination already exists";
			return;
		}
		QDir{}.mkpath(destCollection);  // the destination label may not have a folder on disk yet
		if (!QFile::rename(entryIt->folder, newFolderAbs))
		{
			qWarning() << "Catalog: failed to relocate" << entryIt->folder << "to" << newFolderAbs;
			return;
		}
	}

	// The storage moved but the MediaId (name+size) and its metadata record did not. Strip removedLabelId from
	// the stored list so it can't re-appear via the stored set, and drop dest's id since it is now the
	// (derived) storage label rather than a stored extra.
	QStringList ids = readStoredLabelIds(id);
	bool changed = ids.removeAll(removedLabelId) > 0;
	changed = (ids.removeAll(dest->id) > 0) || changed;
	if (changed)
		writeStoredLabelIds(id, ids);

	// Persist the new folder and update the model entry in place.
	writer.set(id, kFolderField, relativeFolder(newFolderAbs));
	entryIt->folder   = newFolderAbs;
	entryIt->labelIds = computeLabelIds(id, *entryIt);
}

// --- Registry mutations (the label objects) ------------------------------------------------------------

bool Catalog::renameLabel(const QString& labelId, const QString& newDisplayName)
{
	Label* label = mutableLabelById(labelId);
	if (!label)
		return false;
	if (label->isVirtual())
	{
		qWarning() << "Catalog: cannot rename the virtual Best label";
		return false;
	}

	const QString newName = newDisplayName.trimmed();
	if (newName.isEmpty())
		return false;
	if (newName == label->displayName)
		return true;  // nothing to do
	if (newName.compare(PHOTOS_DIR_NAME, Qt::CaseInsensitive) == 0)
	{
		qWarning() << "Catalog: cannot rename a label to the reserved name" << PHOTOS_DIR_NAME;
		return false;
	}

	for (const Label& l : _labels)
		if (&l != label && l.displayName.compare(newName, Qt::CaseInsensitive) == 0)
		{
			qWarning() << "Catalog: cannot rename to" << newName << "- another label already uses that name";
			return false;
		}

	// Rename the backing folders if they exist (a freshly created label may have neither yet): the collection
	// folder and the label's photo dir under <root>/Photos. Every item under them rides along in the directory
	// rename; associations are by id, so nothing else changes. Both destinations are collision-checked before
	// either rename runs, so a refusal never leaves just one of the two renamed.
	const QString oldName = label->displayName;
	const QString oldFolder = rootFolder() + "/" + oldName;
	const QString newFolder = rootFolder() + "/" + newName;
	const QString oldPhotoDir = photosRootFolder() + "/" + oldName;
	const QString newPhotoDir = photosRootFolder() + "/" + newName;
	const bool haveCollectionFolder = QDir(oldFolder).exists();
	const bool havePhotoDir         = QDir(oldPhotoDir).exists();
	if ((haveCollectionFolder && QFileInfo::exists(newFolder)) || (havePhotoDir && QFileInfo::exists(newPhotoDir)))
	{
		qWarning() << "Catalog: cannot rename" << oldName << "to" << newName << "- a folder by that name already exists";
		return false;
	}
	if (haveCollectionFolder && !QFile::rename(oldFolder, newFolder))
	{
		qWarning() << "Catalog: failed to rename folder" << oldFolder << "to" << newFolder;
		return false;
	}
	if (havePhotoDir && !QFile::rename(oldPhotoDir, newPhotoDir))
	{
		qWarning() << "Catalog: failed to rename folder" << oldPhotoDir << "to" << newPhotoDir;
		if (haveCollectionFolder)
			QFile::rename(newFolder, oldFolder);  // roll back so the two dirs don't end up under different names
		return false;
	}

	// The folders moved, so every item stored under oldName now lives under newName: rewrite the stored fields
	// before re-deriving the model (otherwise rebuildIndex would re-seed a stale oldName folder label).
	// Case-insensitive, like labelIdForFolderName: a stored folder whose case drifted from the label's display
	// name still just moved on disk, and skipping it here would leave its stored path pointing at the old name.
	MetadataStore::Writer writer = MetadataStore::instance().beginBatch();  // one store write for the whole rewrite loop instead of one per item
	for (auto it = _mediaItems.cbegin(); it != _mediaItems.cend(); ++it)
	{
		if (storageLabelNameOf(*it).compare(oldName, Qt::CaseInsensitive) != 0)
			continue;
		if (it->type == MediaType::Photo)
		{
			// An owned photo's folder is the renamed dir itself, and its source file sits inside it.
			writer.set(it.key(), kFolderField, relativeFolder(newPhotoDir));
			writer.set(it.key(), kSourcePathField, newPhotoDir + "/" + QFileInfo(it->sourcePath).fileName());
		}
		else
		{
			writer.set(it.key(), kFolderField, relativeFolder(newFolder + "/" + QFileInfo(it->folder).fileName()));
		}
	}

	label->displayName = newName;
	saveRegistry();
	rebuildIndex();  // re-derive the model from the updated store (folder labels now resolve to newName)
	return true;
}

void Catalog::setColor(const QString& labelId, const QString& color)
{
	Label* label = mutableLabelById(labelId);
	if (!label || label->color == color)
		return;
	label->color = color;
	saveRegistry();
}

void Catalog::createLabel(const QString& displayName)
{
	if (ensureFolderLabelExists(displayName))
		saveRegistry();
}

Catalog::DeleteImpact Catalog::deleteLabelImpact(const QString& labelId) const
{
	DeleteImpact impact;
	const Label* label = labelById(labelId);
	if (!label || label->isVirtual())
		return impact;  // Best / unknown id: nothing to delete

	for (const MediaId& id : mediaItemsForLabel(labelId))
	{
		// A carrier is either stored under this label (its folder/photo dir is named after it -> relocate) or
		// merely tags it as an extra (stored elsewhere, or folder-less -> untag).
		const Entry entry = _mediaItems.value(id);
		if (storageLabelIdOf(entry) != labelId)
		{
			++impact.untagCount;
			// A folder-less item (a referenced photo) has no storage label backing it up: untagging its last
			// ordinary label would orphan it just as surely as a blocked relocation would a stored-under item.
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

bool Catalog::deleteLabel(const QString& labelId)
{
	Label* label = mutableLabelById(labelId);
	if (!label || label->isVirtual())
		return false;
	if (deleteLabelImpact(labelId).wouldOrphan)
		return false;

	const QString displayName = label->displayName;

	// Relocate every item stored under the label off it. Collect first: relocateFolderOffLabel mutates _mediaItems.
	QList<MediaId> storedHere;
	for (const MediaId& id : mediaItemsForLabel(labelId))
		if (storageLabelIdOf(_mediaItems.value(id)) == labelId)
			storedHere << id;
	for (const MediaId& id : storedHere)
		relocateFolderOffLabel(id, labelId);

	// Whatever still carries the id now is an extra-tagger (stored elsewhere); strip it from the stored list.
	for (const MediaId& id : mediaItemsForLabel(labelId))
	{
		if (!id.isValid())
			continue;
		QStringList ids = readStoredLabelIds(id);
		if (ids.removeAll(labelId) > 0)
		{
			writeStoredLabelIds(id, ids);
			refreshMediaItemLabels(id);
		}
	}

	// A relocation can be blocked (e.g. a destination name collision) - then a folder still names this label.
	// Don't remove the registry entry in that case: rebuildIndex would just re-seed it from the leftover folder.
	bool stillNamed = false;
	for (const MediaId& id : mediaItemsForLabel(labelId))
		if (storageLabelIdOf(_mediaItems.value(id)) == labelId)
		{
			stillNamed = true;
			break;
		}

	if (!stillNamed)
	{
		QDir collection{ rootFolder() + "/" + displayName };
		if (collection.exists() && collection.isEmpty())  // empty after relocation; guard against nuking stray contents
			collection.removeRecursively();
		QDir photoDir{ photosRootFolder() + "/" + displayName };  // the label's owned-photo dir, same empty-only guard
		if (photoDir.exists() && photoDir.isEmpty())
			photoDir.removeRecursively();
		_labels.removeIf([&labelId](const Label& l) { return l.id == labelId; });
	}

	saveRegistry();
	rebuildIndex();
	return !stillNamed;
}

// --- Legacy migration ----------------------------------------------------------------------------------

QString Catalog::readLegacySourceInfo(const QString& folderPath)
{
	QFile infoFile{ folderPath + "/source_info.txt" };
	if (infoFile.open(QIODevice::ReadOnly))
		return QString::fromUtf8(infoFile.readAll()).trimmed();
	return {};
}

void Catalog::seedCatalogFromSourceInfo()
{
	if (_seededFromSourceInfo)
		return;  // already done once; the disk walk happens only at the initial seed

	MetadataStore& store = MetadataStore::instance();
	int seeded = 0, placeholders = 0, clashes = 0;

	// Up to 2 writes per folder below; one Writer for the whole walk, or each would rewrite the whole
	// store, making this an O(n^2)-bytes-written pass for a large pre-existing library.
	MetadataStore::Writer writer = store.beginBatch();

	forEachFolder(rootFolder(), [&](const QString&, const QString& folderPath) {
		const QString sourcePath = readLegacySourceInfo(folderPath);
		if (sourcePath.isEmpty())
			return;  // no legacy record in this folder; nothing to seed

		// Present source -> a real (name+size) id. Missing/unmounted -> a size-unknown placeholder (!isValid),
		// so the frames still surface under their folder label, exactly as before; the source can be reconciled
		// later by the planned integrity tool.
		const QFileInfo sourceInfo{ sourcePath };
		const MediaId id = sourceInfo.isFile() ? MediaId::fromFile(sourcePath)
		                                       : MediaId::fromNameAndSize(sourceInfo.fileName(), -1);

		const QString folderRel = relativeFolder(folderPath);
		const QString existing  = store.get(id, kFolderField).toString();
		if (!existing.isEmpty() && existing != folderRel)
		{
			// Two folders resolving to the same id (genuine duplicate sources, or two same-named missing
			// sources collapsing to one placeholder). Keep the first; don't silently clobber it.
			qWarning() << "Catalog: seed - id clash, skipping" << folderPath << "(collides with" << absoluteFolder(existing) << ")";
			++clashes;
			return;
		}

		writer.set(id, kSourcePathField, sourcePath);
		writer.set(id, kFolderField, folderRel);
		if (id.isValid())
			++seeded;
		else
			++placeholders;
	});

	_seededFromSourceInfo = true;
	saveRegistry();
	qInfo() << "Catalog: seeded" << seeded << "videos from source_info.txt," << placeholders
	        << "source-unavailable placeholders," << clashes << "clashes skipped";
}

void Catalog::migrateBestTxt()
{
	const QString bestTxt = rootFolder() + "/best.txt";
	QFile file{ bestTxt };
	if (!file.exists() || !file.open(QIODevice::ReadOnly))
		return;

	int migrated = 0, unresolved = 0;

	// One writeStoredLabelIds per line below; without batching, each rewrites the whole store, making a
	// large best.txt migration O(n^2) bytes written (same reasoning as seedCatalogFromSourceInfo's batch).
	BatchScope batch;

	for (const QByteArray& line : file.readAll().split('\n'))
	{
		const QString folderPath = QString::fromUtf8(line).trimmed();
		if (folderPath.isEmpty())
			continue;

		// A stale entry (folder gone) or a video whose source file is missing can't be re-keyed by MediaId.
		const QString sourcePath = QDir(folderPath).exists() ? readLegacySourceInfo(folderPath) : QString{};
		const MediaId id = sourcePath.isEmpty() ? MediaId{} : MediaId::fromFile(sourcePath);
		if (!id.isValid())
		{
			++unresolved;
			continue;
		}

		QStringList ids = readStoredLabelIds(id);
		if (!ids.contains(BestLabelId))
		{
			ids << BestLabelId;
			writeStoredLabelIds(id, ids);
		}
		++migrated;
	}
	file.close();

	// Back up rather than delete: keeps the original recoverable and ensures the migration runs only once.
	if (!QFile::rename(bestTxt, bestTxt + ".pre-labels-backup"))
		qWarning() << "Catalog: best.txt migrated but could not be renamed to a backup;" << bestTxt << "remains";

	qInfo() << "Catalog: best.txt migration - migrated" << migrated << "entries to the Best label," << unresolved << "unresolved (missing folder or source video)";
}
