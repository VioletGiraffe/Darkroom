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

// Reject only a nameless (default-constructed) identity - it has no usable key(). Everything with a name
// persists; the store is a dumb key->fields map and doesn't interpret records.
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
	// A Writer is necessarily alive here (mutations only run through one), so the actual save always
	// happens at the outermost Writer's destruction.
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
		// key() is "<size>:<lowercased-name>"; the size is the numeric prefix before the first ':'. The
		// record's "name" carries the original case (the key's name part is lowercased), so prefer it.
		const QString name = it.value().toObject().value(QStringLiteral("name")).toString();
		const qint64 size = it.key().section(':', 0, 0).toLongLong();
		if (name.isEmpty() || size < 0)
			continue;  // skip a nameless record or a legacy negative-size (source-unavailable) key - neither
			           // resolves to a real catalog identity
		ids.push_back(MediaId::fromNameAndSize(name, size));
	}
	return ids;
}

void MetadataStore::rekey(const MediaId& oldId, const MediaId& newId)
{
	if (!oldId.isValid() || !newId.isValid() || !_records.contains(oldId.key()))
		return;
	// operator== folds case, so equal ids alone are not enough to bail: a case-only rename must still pass
	// through to update the record's original-case "name".
	if (oldId == newId && oldId.name() == newId.name())
		return;

	QJsonObject record = _records.take(oldId.key()).toObject();
	record.insert(QStringLiteral("name"), newId.name()); // record now describes the renamed file
	_records.insert(newId.key(), record);

	scheduleSave();
}
