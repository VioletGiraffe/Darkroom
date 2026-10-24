#include "Core/Library.h"
#include "Core/Catalog.h"
#include "Core/JsonPersistence.h"
#include "Core/MediaItemId.h"
#include "Core/MetadataStore.h"

#include "assert/advanced_assert.h"
#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QSet>
#include <QStringList>
RESTORE_COMPILER_WARNINGS

#include <algorithm>
#include <limits>
#include <optional>
#include <utility>

namespace {

struct LoadedLibraryData
{
	QString rootFolder;
	QJsonObject metadataRecords;
	QJsonObject labelRegistry;
	bool metadataConverted = false;  // from the first format: saved on load
};

[[nodiscard]] QString normalizedRoot(const QString& root)
{
	return QDir(QDir::cleanPath(QDir::fromNativeSeparators(root.trimmed()))).absolutePath();
}

[[nodiscard]] QString labelRegistryValidationError(const QString& path, const QJsonObject& registry)
{
	QSet<qint64> labelIds;
	const QJsonArray labels = registry.value(QStringLiteral("labels")).toArray();
	for (int index = 0; index < labels.size(); ++index)
	{
		if (!labels[index].isObject())
			return QObject::tr("Invalid label registry:\n%1\n\nEntry %2 must be a JSON object.")
				.arg(QDir::toNativeSeparators(path)).arg(index + 1);

		const QJsonObject label = labels[index].toObject();
		const QJsonValue idValue = label.value(QStringLiteral("id"));
		if (!idValue.isDouble())
			return QObject::tr("Invalid label registry:\n%1\n\nEntry %2 must contain a numeric 'id'.")
				.arg(QDir::toNativeSeparators(path)).arg(index + 1);
		const qint64 idFromLowDefault = idValue.toInteger(std::numeric_limits<qint64>::min());
		const qint64 idFromHighDefault = idValue.toInteger(std::numeric_limits<qint64>::max());
		if (idFromLowDefault != idFromHighDefault)
			return QObject::tr("Invalid label registry:\n%1\n\nEntry %2 must contain an integer 'id'.")
				.arg(QDir::toNativeSeparators(path)).arg(index + 1);
		if (!label.value(QStringLiteral("displayName")).isString())
			return QObject::tr("Invalid label registry:\n%1\n\nEntry %2 must contain a string 'displayName'.")
				.arg(QDir::toNativeSeparators(path)).arg(index + 1);
		const QJsonValue color = label.value(QStringLiteral("color"));
		if (!color.isUndefined() && !color.isString())
			return QObject::tr("Invalid label registry:\n%1\n\nEntry %2 has a non-string 'color'.")
				.arg(QDir::toNativeSeparators(path)).arg(index + 1);

		const qint64 id = idFromLowDefault;
		if (labelIds.contains(id))
			return QObject::tr("Invalid label registry:\n%1\n\nEntry %2 repeats label id %3.")
				.arg(QDir::toNativeSeparators(path)).arg(index + 1).arg(id);
		labelIds.insert(id);
	}
	return {};
}

[[nodiscard]] QString metadataValidationError(const QString& path, const QJsonObject& document)
{
	const QJsonValue version = document.value(MetadataStore::VersionField);
	if (version.toInteger(-1) != MetadataStore::FormatVersion)
		return QObject::tr("The catalog was saved in an unsupported format, possibly by a newer version of Darkroom:\n%1")
			.arg(QDir::toNativeSeparators(path));

	const QJsonValue items = document.value(MetadataStore::ItemsField);
	if (!items.isObject())
		return QObject::tr("Invalid catalog:\n%1\n\n'%2' must be a JSON object.").arg(QDir::toNativeSeparators(path), MetadataStore::ItemsField.toString());
	const QJsonObject records = items.toObject();
	for (auto it = records.begin(); it != records.end(); ++it)
	{
		if (mediaItemIdFromString(it.key()) == MediaItemId::None)
			return QObject::tr("Invalid catalog:\n%1\n\n'%2' is not a valid item id.").arg(QDir::toNativeSeparators(path), it.key());
		if (!it.value().isObject())
			return QObject::tr("Invalid catalog:\n%1\n\nItem %2 must be a JSON object.").arg(QDir::toNativeSeparators(path), it.key());
	}
	return {};
}

// The original is kept beside the converted catalog; an existing copy is from an earlier attempt and stays.
[[nodiscard]] bool backUpFirstFormatCatalog(const QString& path)
{
	const QString backupPath = QFileInfo(path).dir().filePath(QStringLiteral("catalog.v1-backup.json"));
	return QFileInfo::exists(backupPath) || QFile::copy(path, backupPath);
}

[[nodiscard]] std::optional<LoadedLibraryData> loadLibraryData(const QString& root, QString* error)
{
	if (error)
		error->clear();

	if (root.trimmed().isEmpty())
	{
		if (error)
			*error = QObject::tr("The library folder cannot be empty.");
		return std::nullopt;
	}

	const QString normalized = normalizedRoot(root);
	const QFileInfo rootInfo(normalized);
	if (rootInfo.exists() && !rootInfo.isDir())
	{
		if (error)
			*error = QObject::tr("The library path is not a folder:\n%1").arg(QDir::toNativeSeparators(normalized));
		return std::nullopt;
	}
	if (!QDir{}.mkpath(normalized))
	{
		if (error)
			*error = QObject::tr("Could not create or access the library folder:\n%1").arg(QDir::toNativeSeparators(normalized));
		return std::nullopt;
	}

	const QString metadataPath = MetadataStore::filePathFor(normalized);
	JsonPersistence::ObjectReadResult metadata = JsonPersistence::readObject(metadataPath);
	if (!metadata.success)
	{
		if (error)
			*error = metadata.error;
		return std::nullopt;
	}
	const QString labelsPath = Catalog::registryFilePathFor(normalized);
	JsonPersistence::ObjectReadResult labels = JsonPersistence::readObject(labelsPath, u"labels");
	if (!labels.success)
	{
		if (error)
			*error = labels.error;
		return std::nullopt;
	}
	const QString labelsValidationError = labelRegistryValidationError(labelsPath, labels.object);
	if (!labelsValidationError.isEmpty())
	{
		if (error)
			*error = labelsValidationError;
		return std::nullopt;
	}

	// The first format has no version field: it is the record object itself.
	if (!metadata.object.isEmpty() && !metadata.object.contains(MetadataStore::VersionField))
	{
		if (!backUpFirstFormatCatalog(metadataPath))
		{
			if (error)
				*error = QObject::tr("The catalog needs converting to the current format, but a backup copy of it could not be created next to it:\n%1")
					.arg(QDir::toNativeSeparators(metadataPath));
			return std::nullopt;
		}
		return LoadedLibraryData{ normalized, Catalog::itemRecordsFromFirstFormat(metadata.object), std::move(labels.object), true };
	}

	if (!metadata.object.isEmpty())
	{
		const QString metadataError = metadataValidationError(metadataPath, metadata.object);
		if (!metadataError.isEmpty())
		{
			if (error)
				*error = metadataError;
			return std::nullopt;
		}
	}
	return LoadedLibraryData{ normalized, metadata.object.value(MetadataStore::ItemsField).toObject(), std::move(labels.object) };
}

} // namespace

class LibraryState
{
public:
	explicit LibraryState(LoadedLibraryData data)
		: rootFolder(std::move(data.rootFolder))
		, metadataStore(rootFolder, std::move(data.metadataRecords))
		, catalog(rootFolder, metadataStore, data.labelRegistry)
	{
		if (data.metadataConverted)
			metadataStore.scheduleSave();
	}

	[[nodiscard]] bool flushPendingWrites(QString* error)
	{
		QString metadataError;
		QString registryError;
		const bool metadataSaved = metadataStore.flushPendingSave(&metadataError);
		const bool registrySaved = catalog.flushPendingRegistrySave(&registryError);
		if (error)
		{
			QStringList errors;
			if (!metadataSaved)
				errors << metadataError;
			if (!registrySaved)
				errors << registryError;
			*error = errors.join("\n\n");
		}
		return metadataSaved && registrySaved;
	}

	[[nodiscard]] QString pendingPersistenceError() const
	{
		QStringList errors;
		if (!metadataStore.pendingSaveError().isEmpty())
			errors << metadataStore.pendingSaveError();
		if (!catalog.pendingRegistrySaveError().isEmpty())
			errors << catalog.pendingRegistrySaveError();
		return errors.join("\n\n");
	}

	void setPersistenceFailureHandler(std::function<void()> handler)
	{
		metadataStore.setPersistenceFailureHandler(handler);
		catalog.setPersistenceFailureHandler(std::move(handler));
	}

	void setCatalogChangeHandler(std::function<void()> handler)
	{
		catalog.setCatalogChangeHandler(std::move(handler));
	}

	void setStorageMoveHandlers(Catalog::StorageMoveHandlers handlers)
	{
		catalog.setStorageMoveHandlers(std::move(handlers));
	}

private:
	friend class Library;

	const QString rootFolder;
	MetadataStore metadataStore;
	Catalog catalog;
};

namespace {

[[nodiscard]] std::unique_ptr<LibraryState> loadLibraryState(const QString& root, QString* error)
{
	std::optional<LoadedLibraryData> data = loadLibraryData(root, error);
	if (!data)
		return {};

	auto state = std::make_unique<LibraryState>(std::move(*data));
	QString saveError;
	if (!state->flushPendingWrites(&saveError))
	{
		if (error)
			*error = QObject::tr("The library was read, but its initial state could not be saved:\n\n%1").arg(saveError);
		return {};
	}
	return state;
}

} // namespace

// LibraryState must be complete where unique_ptr's destructor is instantiated.
Library::Library() = default;
Library::~Library() = default;

bool Library::holdsLibrary(const QString& folder)
{
	if (folder.trimmed().isEmpty())
		return false;

	const QString normalized = normalizedRoot(folder);
	return std::ranges::any_of(persistenceFilePaths(normalized), [](const QString& path) { return QFileInfo::exists(path); });
}

QStringList Library::persistenceFilePaths(const QString& root)
{
	return { MetadataStore::filePathFor(root), Catalog::registryFilePathFor(root) };
}

bool Library::setRoot(const QString& root, QString* error)
{
	QString currentSaveError;
	if (!flushPendingWrites(&currentSaveError))
	{
		if (error)
			*error = QObject::tr("The current library still has unsaved changes and cannot be replaced:\n\n%1").arg(currentSaveError);
		return false;
	}

	std::unique_ptr<LibraryState> replacement = loadLibraryState(root, error);
	if (!replacement)
		return false;
	replacement->setPersistenceFailureHandler(_persistenceFailureHandler);
	replacement->setCatalogChangeHandler([this] { emit catalogChanged(); });
	replacement->setStorageMoveHandlers(_storageMoveHandlers);
	_state = std::move(replacement);
	++_generation;
	qInfo() << "Library: opened, catalog folder" << QDir::toNativeSeparators(_state->rootFolder);
	emit catalogChanged();
	return true;
}

// Empty access is a programming error; startup callers gate it with isLoaded().
const QString& Library::rootFolder() const
{
	assert_r(_state);
	return _state->rootFolder;
}

QString Library::photosRootFolder() const
{
	return rootFolder() + "/" + Catalog::PhotosDirectoryName.toString();
}

Catalog& Library::catalog()
{
	assert_r(_state);
	return _state->catalog;
}

const Catalog& Library::catalog() const
{
	assert_r(_state);
	return _state->catalog;
}

MetadataStore& Library::metadataStore()
{
	assert_r(_state);
	return _state->metadataStore;
}

const MetadataStore& Library::metadataStore() const
{
	assert_r(_state);
	return _state->metadataStore;
}

bool Library::flushPendingWrites(QString* error)
{
	return _state ? _state->flushPendingWrites(error) : true;
}

QString Library::pendingPersistenceError() const
{
	return _state ? _state->pendingPersistenceError() : QString{};
}

void Library::setPersistenceFailureHandler(std::function<void()> handler)
{
	_persistenceFailureHandler = std::move(handler);
	if (_state)
		_state->setPersistenceFailureHandler(_persistenceFailureHandler);
	if (_persistenceFailureHandler && !pendingPersistenceError().isEmpty())
		_persistenceFailureHandler();
}

void Library::setStorageMoveHandlers(Catalog::StorageMoveHandlers handlers)
{
	_storageMoveHandlers = std::move(handlers);
	if (_state)
		_state->setStorageMoveHandlers(_storageMoveHandlers);
}
