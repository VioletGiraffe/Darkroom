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
	constexpr QStringView kSourceVideoPathField = u"sourceVideoPath";
	constexpr QStringView kSplitIntoFramesField = u"splitIntoFrames";

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

	// Folder labels come from the collections videos actually live in (the distinct collection segment of each
	// video's folder), not from a disk walk - so an empty collection folder no longer yields a label.
	MetadataStore& store = MetadataStore::instance();
	QSet<QString> collections;
	for (const VideoId& id : store.allVideoIds())
	{
		const QString folderRel = store.get(id, kFolderField).toString();
		if (!folderRel.isEmpty())
			collections.insert(collectionNameOf(absoluteFolder(folderRel)));
	}
	for (const QString& collection : collections)
		changed = ensureFolderLabelExists(collection) || changed;

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

// --- Per-video stored label ids (MetadataStore "labels" field) ------------------------------------------

QStringList Catalog::readStoredLabelIds(const VideoId& id)
{
	const QJsonArray arr = MetadataStore::instance().get(id, kLabelsField).toArray();
	QStringList out;
	out.reserve(arr.size());
	for (const QJsonValue& v : arr)
		out << v.toString();
	return out;
}

void Catalog::writeStoredLabelIds(const VideoId& id, const QStringList& labelIds)
{
	QJsonArray arr;
	for (const QString& labelId : labelIds)
		arr.append(labelId);
	MetadataStore::instance().set(id, kLabelsField, arr);
}

// --- Folder helpers ------------------------------------------------------------------------------------

QString Catalog::collectionNameOf(const QString& folderAbs)
{
	// The frame folder lives at <root>/<collection>/<videoFolder>; the collection-folder name is the
	// display name of this video's folder label.
	return QFileInfo(folderAbs).dir().dirName();
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

QStringList Catalog::computeLabelIds(const VideoId& id, const QString& folderAbs) const
{
	QStringList labelIds;

	const QString folderLabelId = labelIdForFolderName(collectionNameOf(folderAbs));
	if (!folderLabelId.isEmpty())
		labelIds << folderLabelId;

	for (const QString& stored : readStoredLabelIds(id))
		if (!labelIds.contains(stored))
			labelIds << stored;

	return labelIds;
}

void Catalog::rebuildIndex()
{
	ensureBestAndFolderLabels();  // every collection a video lives in + Best has a registry label before labels resolve

	_videos.clear();
	MetadataStore& store = MetadataStore::instance();
	for (const VideoId& id : store.allVideoIds())
	{
		const QString folderRel = store.get(id, kFolderField).toString();
		if (folderRel.isEmpty())
			continue;  // a record with no folder isn't a catalog video (e.g. a legacy orphan carrying only labels)

		Entry e;
		e.folder          = absoluteFolder(folderRel);
		e.sourceVideoPath = store.get(id, kSourceVideoPathField).toString();
		e.labelIds        = computeLabelIds(id, e.folder);
		e.splitIntoFrames = store.get(id, kSplitIntoFramesField).toBool(true);  // absent -> pre-existing, already split
		_videos.insert(id, e);
	}
}

void Catalog::refreshVideoLabels(const VideoId& id)
{
	const auto it = _videos.find(id);
	if (it != _videos.end())
		it->labelIds = computeLabelIds(id, it->folder);
}

// --- Queries (VideoId-anchored) ------------------------------------------------------------------------

QStringList Catalog::labelsForVideo(const VideoId& id) const
{
	const auto it = _videos.constFind(id);
	return it != _videos.cend() ? it->labelIds : QStringList{};
}

QSet<VideoId> Catalog::videosForLabel(const QString& labelId) const
{
	QSet<VideoId> out;
	for (auto it = _videos.cbegin(); it != _videos.cend(); ++it)
		if (it->labelIds.contains(labelId))
			out.insert(it.key());
	return out;
}

bool Catalog::videoHasLabel(const VideoId& id, const QString& labelId) const
{
	const auto it = _videos.constFind(id);
	return it != _videos.cend() && it->labelIds.contains(labelId);
}

QHash<QString, int> Catalog::labelVideoCounts() const
{
	QHash<QString, int> counts;
	for (auto it = _videos.cbegin(); it != _videos.cend(); ++it)
		for (const QString& labelId : it->labelIds)
			++counts[labelId];
	return counts;
}

QString Catalog::folderForVideo(const VideoId& id) const
{
	return _videos.value(id).folder;
}

QString Catalog::sourceVideoPathForVideo(const VideoId& id) const
{
	return _videos.value(id).sourceVideoPath;
}

bool Catalog::isSplitIntoFrames(const VideoId& id) const
{
	return _videos.value(id).splitIntoFrames;
}

QString Catalog::anySourceVideoDir() const
{
	for (auto it = _videos.cbegin(); it != _videos.cend(); ++it)
		if (!it->sourceVideoPath.isEmpty() && QFileInfo::exists(it->sourceVideoPath))
			return QFileInfo(it->sourceVideoPath).absolutePath();
	return {};
}

// --- Per-video membership (VideoId-anchored) -----------------------------------------------------------

void Catalog::addLabel(const VideoId& id, const QString& labelId)
{
	if (!id.isValid())
	{
		qWarning() << "Catalog: cannot add label" << labelId << "- invalid video id (source video missing?)";
		return;
	}
	QStringList ids = readStoredLabelIds(id);
	if (!ids.contains(labelId))
	{
		ids << labelId;
		writeStoredLabelIds(id, ids);
		refreshVideoLabels(id);
	}
}

void Catalog::removeLabel(const VideoId& id, const QString& labelId)
{
	if (!id.isValid())
	{
		qWarning() << "Catalog: cannot remove label" << labelId << "- invalid video id (source video missing?)";
		return;
	}

	// Removing the ordinary label that names this video's storage folder is a folder relocation, not a metadata
	// edit. A virtual label (Best) never names a folder, so it always falls through to the stored-id strip below.
	const Label* label = labelById(labelId);
	if (label && !label->isVirtual())
	{
		const QString folderAbs = folderForVideo(id);
		if (!folderAbs.isEmpty() && labelIdForFolderName(collectionNameOf(folderAbs)) == labelId)
		{
			relocateFolderOffLabel(id, labelId);
			return;
		}
	}

	QStringList ids = readStoredLabelIds(id);
	if (ids.removeAll(labelId) > 0)
	{
		writeStoredLabelIds(id, ids);
		refreshVideoLabels(id);
	}
}

Catalog::BatchScope::BatchScope()
{
	MetadataStore::instance().beginBatch();
}

Catalog::BatchScope::~BatchScope()
{
	MetadataStore::instance().endBatch();
}

// --- Video lifecycle -----------------------------------------------------------------------------------

bool Catalog::addVideo(const VideoId& id, const QString& sourceVideoPath, const QString& folderAbs, bool splitIntoFrames)
{
	if (!id.isValid())
	{
		qWarning() << "Catalog: cannot add video with an invalid id, source" << sourceVideoPath;
		return false;
	}

	// A name+size collision with a video already tracked under a *different* folder is a genuine duplicate
	// (or two distinct files that happen to collide) - refuse rather than silently overwriting the existing
	// entry's folder, which would orphan that folder and its labels. Re-registering the same id at the same
	// folder (re-export) is not a collision and falls through normally.
	const auto existing = _videos.constFind(id);
	if (existing != _videos.constEnd() && existing->folder != folderAbs)
	{
		qWarning() << "Catalog: refusing to add video, id" << id.key() << "is already tracked at" << existing->folder
		           << "- collides with" << sourceVideoPath;
		return false;
	}

	MetadataStore& store = MetadataStore::instance();
	store.set(id, kSourceVideoPathField, sourceVideoPath);
	store.set(id, kFolderField, relativeFolder(folderAbs));
	store.set(id, kSplitIntoFramesField, splitIntoFrames);

	if (ensureFolderLabelExists(collectionNameOf(folderAbs)))
		saveRegistry();

	Entry e;
	e.folder          = folderAbs;
	e.sourceVideoPath = sourceVideoPath;
	e.labelIds        = computeLabelIds(id, folderAbs);
	e.splitIntoFrames = splitIntoFrames;
	_videos.insert(id, e);
	return true;
}

void Catalog::removeVideo(const VideoId& id)
{
	MetadataStore::instance().remove(id);
	_videos.remove(id);
}

bool Catalog::applyRename(const VideoId& oldId, const VideoId& newId, const QString& newSourceVideoPath, const QString& newFolderAbs)
{
	// A rename landing on an id already tracked as a *different* video (the new name + size matching one
	// elsewhere in the library) would silently overwrite that entry and orphan its folder - refuse instead,
	// mirroring addVideo. oldId == newId (a folder-only rename) is the entry itself, never a collision.
	if (oldId != newId && _videos.contains(newId))
	{
		qWarning() << "Catalog: refusing to rename to id" << newId.key() << "- already tracked at" << _videos.value(newId).folder;
		return false;
	}

	MetadataStore& store = MetadataStore::instance();
	store.rekey(oldId, newId);  // no-op when ids are equal; carries loop intervals + labels to the new identity
	store.set(newId, kSourceVideoPathField, newSourceVideoPath);
	store.set(newId, kFolderField, relativeFolder(newFolderAbs));

	const bool splitIntoFrames = _videos.value(oldId).splitIntoFrames;
	_videos.remove(oldId);
	Entry e;
	e.folder          = newFolderAbs;
	e.sourceVideoPath = newSourceVideoPath;
	e.labelIds        = computeLabelIds(newId, newFolderAbs);
	e.splitIntoFrames = splitIntoFrames;
	_videos.insert(newId, e);
	return true;
}

// --- Integrity check -------------------------------------------------------------------------------------

Catalog::IntegrityReport Catalog::scanIntegrity() const
{
	IntegrityReport report;

	for (auto it = _videos.cbegin(); it != _videos.cend(); ++it)
	{
		// Relinkable: a placeholder (missing source at seed/ingestion time) whose recorded path now exists.
		if (!it.key().isValid())
		{
			if (!it->sourceVideoPath.isEmpty() && QFileInfo::exists(it->sourceVideoPath))
				report.relinkable.push_back({ it.key(), it->folder, it->sourceVideoPath });
			continue;
		}

		// Ghost: a tracked, fully-split video whose frame folder has no images left. A video still awaiting its
		// on-demand split legitimately has no real frames yet (only preview/ ones) - see isSplitIntoFrames.
		if (it->splitIntoFrames && QDir(it->folder).entryList(IMAGE_FILE_FILTERS, QDir::Files).isEmpty())
		{
			GhostEntry ghost;
			ghost.id             = it.key();
			ghost.folder         = it->folder;
			ghost.sourceVideoPath = it->sourceVideoPath;
			ghost.sourcePresent  = !it->sourceVideoPath.isEmpty() && QFileInfo::exists(it->sourceVideoPath);
			report.ghosts.push_back(ghost);
		}
	}

	// Untracked: every non-empty frame folder on disk that isn't any entry's folder.
	QSet<QString> knownFolders;
	for (auto it = _videos.cbegin(); it != _videos.cend(); ++it)
		knownFolders.insert(it->folder);

	forEachFolder(rootFolder(), [&](const QString&, const QString& folderPath) {
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
			const VideoId candidateId = VideoId::fromFile(recorded);
			const auto existing = _videos.constFind(candidateId);
			if (existing != _videos.cend())
			{
				u.clashId = candidateId;
				u.filesIdentical = filesAreIdentical(recorded, existing->sourceVideoPath);
			}
		}
		report.untracked.push_back(u);
	});

	return report;
}

bool Catalog::relinkPlaceholder(const VideoId& placeholderId, const QString& confirmedSourcePath)
{
	if (placeholderId.isValid())
	{
		qWarning() << "Catalog: relinkPlaceholder called with a non-placeholder id";
		return false;
	}
	const VideoId realId = VideoId::fromFile(confirmedSourcePath);
	if (!realId.isValid())
	{
		qWarning() << "Catalog: cannot relink - no file at" << confirmedSourcePath;
		return false;
	}

	const auto placeholderEntry = _videos.constFind(placeholderId);
	if (placeholderEntry == _videos.cend())
	{
		qWarning() << "Catalog: relinkPlaceholder - unknown placeholder id" << placeholderId.key();
		return false;
	}
	const QString folderAbs = placeholderEntry->folder;

	// A name+size collision with a video already tracked under a *different* folder is a separate, unrelated
	// problem (not the placeholder being relinked) - refuse rather than clobbering that entry, mirroring addVideo.
	const auto existingReal = _videos.constFind(realId);
	if (existingReal != _videos.cend() && existingReal->folder != folderAbs)
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

	MetadataStore& store = MetadataStore::instance();
	store.remove(placeholderId);
	store.set(realId, kSourceVideoPathField, confirmedSourcePath);
	store.set(realId, kFolderField, relativeFolder(folderAbs));
	store.set(realId, kSplitIntoFramesField, splitIntoFrames);
	writeStoredLabelIds(realId, labels);

	_videos.remove(placeholderId);
	Entry e;
	e.folder          = folderAbs;
	e.sourceVideoPath = confirmedSourcePath;
	e.labelIds        = computeLabelIds(realId, folderAbs);
	e.splitIntoFrames = splitIntoFrames;
	_videos.insert(realId, e);
	return true;
}

void Catalog::relocateFolderOffLabel(const VideoId& id, const QString& removedLabelId)
{
	const QString folderAbs = folderForVideo(id);

	// Destination = the alphabetically-first of the video's *remaining* ordinary (non-virtual) labels.
	const Label* dest = nullptr;
	for (const QString& labelId : labelsForVideo(id))
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
		qWarning() << "Catalog: refusing to remove the last ordinary label from" << folderAbs << "- a video must stay in some folder";
		return;
	}

	const QString videoFolderName = QFileInfo(folderAbs).fileName();
	const QString destCollection  = rootFolder() + "/" + dest->displayName;
	const QString newFolderAbs    = destCollection + "/" + videoFolderName;
	if (QFileInfo::exists(newFolderAbs))
	{
		qWarning() << "Catalog: cannot relocate" << folderAbs << "to" << newFolderAbs << "- destination already exists";
		return;
	}

	QDir{}.mkpath(destCollection);  // the destination label may not have a folder on disk yet
	if (!QFile::rename(folderAbs, newFolderAbs))
	{
		qWarning() << "Catalog: failed to relocate" << folderAbs << "to" << newFolderAbs;
		return;
	}

	// The frame folder moved but the source video (hence the VideoId and its metadata record) did not. Strip
	// removedLabelId from the stored list so it can't re-appear via the stored set, and drop dest's id since it
	// is now the (derived) folder label rather than a stored extra.
	QStringList ids = readStoredLabelIds(id);
	bool changed = ids.removeAll(removedLabelId) > 0;
	changed = (ids.removeAll(dest->id) > 0) || changed;
	if (changed)
		writeStoredLabelIds(id, ids);

	// Persist the new folder and update the model entry in place.
	MetadataStore::instance().set(id, kFolderField, relativeFolder(newFolderAbs));
	const auto it = _videos.find(id);
	if (it != _videos.end())
	{
		it->folder   = newFolderAbs;
		it->labelIds = computeLabelIds(id, newFolderAbs);
	}
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

	for (const Label& l : _labels)
		if (&l != label && l.displayName.compare(newName, Qt::CaseInsensitive) == 0)
		{
			qWarning() << "Catalog: cannot rename to" << newName << "- another label already uses that name";
			return false;
		}

	// Rename the backing collection folder if it exists (a freshly created label may have none yet). Every video
	// under it rides along in one directory rename; associations are by id, so nothing else changes.
	const QString oldName = label->displayName;
	const QString oldFolder = rootFolder() + "/" + oldName;
	const QString newFolder = rootFolder() + "/" + newName;
	if (QDir(oldFolder).exists())
	{
		if (QFileInfo::exists(newFolder))
		{
			qWarning() << "Catalog: cannot rename - a folder already exists at" << newFolder;
			return false;
		}
		if (!QFile::rename(oldFolder, newFolder))
		{
			qWarning() << "Catalog: failed to rename folder" << oldFolder << "to" << newFolder;
			return false;
		}
	}

	// The collection folder moved, so every video stored in it now lives under newName: rewrite the stored folder
	// of each before re-deriving the model (otherwise rebuildIndex would re-seed a stale oldName folder label).
	// Case-insensitive, like labelIdForFolderName: a stored folder whose case drifted from the label's display
	// name still just moved on disk, and skipping it here would leave its stored path pointing at the old name.
	MetadataStore& store = MetadataStore::instance();
	for (auto it = _videos.cbegin(); it != _videos.cend(); ++it)
		if (collectionNameOf(it->folder).compare(oldName, Qt::CaseInsensitive) == 0)
		{
			const QString movedFolderAbs = newFolder + "/" + QFileInfo(it->folder).fileName();
			store.set(it.key(), kFolderField, relativeFolder(movedFolderAbs));
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

	for (const VideoId& id : videosForLabel(labelId))
	{
		// A carrier is either stored under this label (its folder is named after it -> relocate) or merely
		// tags it as an extra (stored elsewhere -> untag).
		if (labelIdForFolderName(collectionNameOf(folderForVideo(id))) != labelId)
		{
			++impact.untagCount;
			continue;
		}

		++impact.relocateCount;
		bool hasOtherOrdinary = false;
		for (const QString& other : labelsForVideo(id))
		{
			if (other == labelId)
				continue;
			const Label* l = labelById(other);
			if (l && !l->isVirtual())
			{
				hasOtherOrdinary = true;
				break;
			}
		}
		if (!hasOtherOrdinary)
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

	// Relocate every video stored under the label off it. Collect first: relocateFolderOffLabel mutates _videos.
	QList<VideoId> storedHere;
	for (const VideoId& id : videosForLabel(labelId))
		if (labelIdForFolderName(collectionNameOf(folderForVideo(id))) == labelId)
			storedHere << id;
	for (const VideoId& id : storedHere)
		relocateFolderOffLabel(id, labelId);

	// Whatever still carries the id now is an extra-tagger (stored elsewhere); strip it from the stored list.
	for (const VideoId& id : videosForLabel(labelId))
	{
		if (!id.isValid())
			continue;
		QStringList ids = readStoredLabelIds(id);
		if (ids.removeAll(labelId) > 0)
		{
			writeStoredLabelIds(id, ids);
			refreshVideoLabels(id);
		}
	}

	// A relocation can be blocked (e.g. a destination name collision) - then a folder still names this label.
	// Don't remove the registry entry in that case: rebuildIndex would just re-seed it from the leftover folder.
	bool stillNamed = false;
	for (const VideoId& id : videosForLabel(labelId))
		if (labelIdForFolderName(collectionNameOf(folderForVideo(id))) == labelId)
		{
			stillNamed = true;
			break;
		}

	if (!stillNamed)
	{
		QDir collection{ rootFolder() + "/" + displayName };
		if (collection.exists() && collection.isEmpty())  // empty after relocation; guard against nuking stray contents
			collection.removeRecursively();
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

	// Up to 2 store.set() calls per folder below; without batching, each rewrites the whole store, making
	// this an O(n^2)-bytes-written pass for a large pre-existing library.
	BatchScope batch;

	forEachFolder(rootFolder(), [&](const QString&, const QString& folderPath) {
		const QString sourcePath = readLegacySourceInfo(folderPath);
		if (sourcePath.isEmpty())
			return;  // no legacy record in this folder; nothing to seed

		// Present source -> a real (name+size) id. Missing/unmounted -> a size-unknown placeholder (!isValid),
		// so the frames still surface under their folder label, exactly as before; the source can be reconciled
		// later by the planned integrity tool.
		const QFileInfo sourceInfo{ sourcePath };
		const VideoId id = sourceInfo.isFile() ? VideoId::fromFile(sourcePath)
		                                       : VideoId::fromNameAndSize(sourceInfo.fileName(), -1);

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

		store.set(id, kSourceVideoPathField, sourcePath);
		store.set(id, kFolderField, folderRel);
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

		// A stale entry (folder gone) or a video whose source file is missing can't be re-keyed by VideoId.
		const QString sourcePath = QDir(folderPath).exists() ? readLegacySourceInfo(folderPath) : QString{};
		const VideoId id = sourcePath.isEmpty() ? VideoId{} : VideoId::fromFile(sourcePath);
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
