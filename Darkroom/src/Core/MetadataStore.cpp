#include "Core/MetadataStore.h"
#include "Core/JsonPersistence.h"
#include "assert/advanced_assert.h"
#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QDebug>
RESTORE_COMPILER_WARNINGS

#include <utility>

MetadataStore::MetadataStore(QString rootFolder, QJsonObject records)
	: _rootFolder(std::move(rootFolder)), _records(std::move(records))
{}

QString MetadataStore::filePathFor(const QString& rootFolder)
{
	return rootFolder + "/catalog.json";
}

QString MetadataStore::writeRecords() const
{
	QJsonObject document;
	document.insert(VersionField.toString(), FormatVersion);
	document.insert(ItemsField.toString(), _records);
	return JsonPersistence::writeObject(filePathFor(_rootFolder), document);
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

QJsonValue MetadataStore::get(MediaItemId id, QStringView field) const
{
	return _records.value(toString(id)).toObject().value(field.toString());
}

void MetadataStore::set(MediaItemId id, QStringView field, const QJsonValue& value)
{
	assert_and_return_r(id != MediaItemId::None, );

	const QString key = toString(id);
	QJsonObject record = _records.value(key).toObject();
	record.insert(field.toString(), value);
	_records.insert(key, record);

	scheduleSave();
}

void MetadataStore::removeField(MediaItemId id, QStringView field)
{
	const QString key = toString(id);
	auto it = _records.find(key);
	if (it == _records.end())
		return;

	QJsonObject record = it.value().toObject();
	if (record.take(field.toString()).isUndefined())
		return;

	if (record.isEmpty())
		_records.remove(key);
	else
		_records.insert(key, record);

	scheduleSave();
}

void MetadataStore::remove(MediaItemId id)
{
	if (!_records.take(toString(id)).isUndefined())
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

void MetadataStore::Writer::set(MediaItemId id, QStringView field, const QJsonValue& value)
{
	_store.set(id, field, value);
}

void MetadataStore::Writer::removeField(MediaItemId id, QStringView field)
{
	_store.removeField(id, field);
}

void MetadataStore::Writer::remove(MediaItemId id)
{
	_store.remove(id);
}

// Library validates every key before constructing the store.
std::vector<MediaItemId> MetadataStore::allItemIds() const
{
	std::vector<MediaItemId> ids;
	ids.reserve(_records.size());
	for (auto it = _records.begin(); it != _records.end(); ++it)
		ids.push_back(mediaItemIdFromString(it.key()));
	return ids;
}
