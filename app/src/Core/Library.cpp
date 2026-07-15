#include "Core/Library.h"
#include "Core/Catalog.h"
#include "Core/JsonPersistence.h"
#include "Core/MetadataStore.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QSet>
#include <QStringList>

#include <limits>
#include <optional>
#include <utility>

namespace {

struct LoadedLibraryData
{
	QString rootFolder;
	QJsonObject metadataRecords;
	QJsonObject labelRegistry;
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

	JsonPersistence::ObjectReadResult metadata = JsonPersistence::readObject(normalized + "/catalog.json");
	if (!metadata.success)
	{
		if (error)
			*error = metadata.error;
		return std::nullopt;
	}
	const QString labelsPath = normalized + "/labels.json";
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

	return LoadedLibraryData{ normalized, std::move(metadata.object), std::move(labels.object) };
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

std::optional<Library> Library::load(const QString& root, QString* error)
{
	std::unique_ptr<LibraryState> state = loadLibraryState(root, error);
	if (!state)
		return std::nullopt;
	return Library(std::move(state));
}

Library::Library(std::unique_ptr<LibraryState> state)
	: m_state(std::move(state))
{}

Library::Library(Library&&) noexcept = default;
Library::~Library() = default;

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
	replacement->setPersistenceFailureHandler(m_persistenceFailureHandler);
	m_state = std::move(replacement);
	++m_generation;
	return true;
}

const QString& Library::rootFolder() const
{
	return m_state->rootFolder;
}

QString Library::photosRootFolder() const
{
	return rootFolder() + "/" + Catalog::PhotosDirectoryName.toString();
}

Catalog& Library::catalog()
{
	return m_state->catalog;
}

const Catalog& Library::catalog() const
{
	return m_state->catalog;
}

MetadataStore& Library::metadataStore()
{
	return m_state->metadataStore;
}

const MetadataStore& Library::metadataStore() const
{
	return m_state->metadataStore;
}

bool Library::flushPendingWrites(QString* error)
{
	return m_state->flushPendingWrites(error);
}

QString Library::pendingPersistenceError() const
{
	return m_state->pendingPersistenceError();
}

void Library::setPersistenceFailureHandler(std::function<void()> handler)
{
	m_persistenceFailureHandler = std::move(handler);
	m_state->setPersistenceFailureHandler(m_persistenceFailureHandler);
	if (m_persistenceFailureHandler && !pendingPersistenceError().isEmpty())
		m_persistenceFailureHandler();
}
