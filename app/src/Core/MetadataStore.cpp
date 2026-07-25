#include "Core/MetadataStore.h"
#include "Core/JsonPersistence.h"

#include <QDebug>

#include <utility>

namespace { constexpr const char* kFileName = "catalog.json"; }

MetadataStore::MetadataStore(QString rootFolder, QJsonObject records)
	: _rootFolder(std::move(rootFolder)), _records(std::move(records))
{}

QString MetadataStore::filePath() const
{
	return _rootFolder + "/" + kFileName;
}

QString MetadataStore::writeRecords() const
{
	return JsonPersistence::writeObject(filePath(), _records);
}

bool MetadataStore::flushPendingSave(QString* error)
{
	if (error)
		error->clear();
	if (!_dirty)
		return true;

	const QString saveError = writeRecords();
	if (saveError.isEmpty())
	{
		_dirty = false;
		_pendingSaveError.clear();
		return true;
	}

	const bool firstFailure = _pendingSaveError.isEmpty();
	_pendingSaveError = saveError;
	if (error)
		*error = saveError;
	qWarning() << "MetadataStore: save failed:" << saveError;
	if (firstFailure && _persistenceFailureHandler)
		_persistenceFailureHandler();
	return false;
}

// A nameless default id has no usable key; the store does not otherwise interpret identities.
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
	record.insert(QStringLiteral("name"), id.name());
	record.insert(field.toString(), value);
	_records.insert(id.key(), record);

	scheduleSave();
}

void MetadataStore::removeField(const MediaId& id, QStringView field)
{
	if (id.name().isEmpty())
		return;

	const QString key = id.key();
	auto it = _records.find(key);
	if (it == _records.end())
		return;

	QJsonObject record = it.value().toObject();
	if (record.take(field.toString()).isUndefined())
		return;

	// A name-only record would become a phantom catalog item through allMediaIds().
	if (record.size() <= 1)
		_records.remove(key);
	else
		_records.insert(key, record);

	scheduleSave();
}

void MetadataStore::remove(const MediaId& id)
{
	if (!_records.take(id.key()).isUndefined())
		scheduleSave();
}

void MetadataStore::scheduleSave()
{
	_dirty = true;
}

MetadataStore::Writer MetadataStore::beginBatch()
{
	return Writer(*this);
}

MetadataStore::Writer::Writer(MetadataStore& store)
	: _store(store)
{
	++_store._batchDepth;
}

MetadataStore::Writer::~Writer()
{
	if (--_store._batchDepth == 0 && _store._dirty)
		static_cast<void>(_store.flushPendingSave());
}

void MetadataStore::Writer::set(const MediaId& id, QStringView field, const QJsonValue& value)
{
	_store.set(id, field, value);
}

void MetadataStore::Writer::removeField(const MediaId& id, QStringView field)
{
	_store.removeField(id, field);
}

void MetadataStore::Writer::remove(const MediaId& id)
{
	_store.remove(id);
}

void MetadataStore::Writer::rekey(const MediaId& oldId, const MediaId& newId)
{
	_store.rekey(oldId, newId);
}

std::vector<MediaId> MetadataStore::allMediaIds() const
{
	std::vector<MediaId> ids;
	ids.reserve(_records.size());
	for (auto it = _records.begin(); it != _records.end(); ++it)
	{
		// key() contributes size; the reserved field preserves original-case name.
		const QString name = it.value().toObject().value(QStringLiteral("name")).toString();
		const qint64 size = it.key().section(':', 0, 0).toLongLong();
		if (name.isEmpty() || size < 0)
			continue;
		ids.push_back(MediaId::fromNameAndSize(name, size));
	}
	return ids;
}

void MetadataStore::rekey(const MediaId& oldId, const MediaId& newId)
{
	if (!oldId.isValid() || !newId.isValid() || !_records.contains(oldId.key()))
		return;
	// A case-only rename still updates the reserved original-case name.
	if (oldId == newId && oldId.name() == newId.name())
		return;

	QJsonObject record = _records.take(oldId.key()).toObject();
	record.insert(QStringLiteral("name"), newId.name());
	_records.insert(newId.key(), record);

	scheduleSave();
}
