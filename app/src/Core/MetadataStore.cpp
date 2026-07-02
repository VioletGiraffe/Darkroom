#include "Core/MetadataStore.h"
#include "Utils.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>

namespace { constexpr const char* kFileName = "catalog.json"; }

MetadataStore& MetadataStore::instance()
{
	static MetadataStore store;
	return store;
}

MetadataStore::MetadataStore()
{
	load();
}

QString MetadataStore::filePath() const
{
	return rootFolder() + "/" + kFileName;
}

void MetadataStore::load()
{
	QFile file{ filePath() };
	if (!file.open(QIODevice::ReadOnly))
		return; // No store yet - start empty; it gets created on the first save.

	_records = QJsonDocument::fromJson(file.readAll()).object();
}

void MetadataStore::save() const
{
	QDir{}.mkpath(rootFolder()); // the collection root should already exist, but don't fail a write if it doesn't

	QSaveFile file{ filePath() };
	if (!file.open(QIODevice::WriteOnly))
		return;

	file.write(QJsonDocument{ _records }.toJson(QJsonDocument::Indented));
	file.commit();
}

// Guarded on a non-empty name rather than isValid(): a source-unavailable placeholder (negative size, so
// !isValid()) still has a stable key() and must be able to persist its folder/source-path fields. Only a
// truly empty identity (no name) is rejected.
QJsonValue MetadataStore::get(const MediaId& id, QStringView field) const
{
	if (id.name().isEmpty())
		return {};

	return _records.value(id.key()).toObject().value(field.toString());
}

void MetadataStore::set(const MediaId& id, QStringView field, const QJsonValue& value)
{
	if (id.name().isEmpty())
		return;

	QJsonObject record = _records.value(id.key()).toObject();
	record.insert(QStringLiteral("name"), id.name()); // keep a human-readable name in each record (debugging / future catalog display)
	record.insert(field.toString(), value);
	_records.insert(id.key(), record);

	scheduleSave();
}

void MetadataStore::remove(const MediaId& id)
{
	// QJsonObject::remove returns void; take() removes and hands back the old value (undefined if it wasn't
	// there), so it doubles as the "was anything actually removed?" check that avoids a needless save.
	if (!_records.take(id.key()).isUndefined())
		scheduleSave();
}

void MetadataStore::scheduleSave()
{
	if (_batchDepth > 0)
		_dirty = true;
	else
		save();
}

void MetadataStore::beginBatch()
{
	++_batchDepth;
}

void MetadataStore::endBatch()
{
	if (--_batchDepth == 0 && _dirty)
	{
		_dirty = false;
		save();
	}
}

QList<MediaId> MetadataStore::allMediaIds() const
{
	QList<MediaId> ids;
	ids.reserve(_records.size());
	for (auto it = _records.begin(); it != _records.end(); ++it)
	{
		// key() is "<size>:<lowercased-name>"; the size is the numeric prefix before the first ':'. The
		// record's "name" carries the original case (the key's name part is lowercased), so prefer it.
		const QString name = it.value().toObject().value(QStringLiteral("name")).toString();
		const qint64 size = it.key().section(':', 0, 0).toLongLong();
		if (!name.isEmpty())
			ids.append(MediaId::fromNameAndSize(name, size));
	}
	return ids;
}

void MetadataStore::rekey(const MediaId& oldId, const MediaId& newId)
{
	if (!oldId.isValid() || !newId.isValid() || oldId == newId || !_records.contains(oldId.key()))
		return;

	QJsonObject record = _records.take(oldId.key()).toObject();
	record.insert(QStringLiteral("name"), newId.name()); // record now describes the renamed file
	_records.insert(newId.key(), record);

	scheduleSave();
}
