#include "Core/Library.h"
#include "Core/Catalog.h"
#include "Core/MetadataStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QObject>

#include <optional>
#include <utility>

namespace {

[[nodiscard]] QString normalizedRoot(const QString& root)
{
	return QDir(QDir::cleanPath(QDir::fromNativeSeparators(root.trimmed()))).absolutePath();
}

[[nodiscard]] bool validateJsonObject(const QString& path, const QString& requiredArrayField, QString* error)
{
	if (!QFileInfo::exists(path))
		return true;

	QFile file(path);
	if (!file.open(QIODevice::ReadOnly))
	{
		if (error)
			*error = QObject::tr("Could not read:\n%1\n\n%2").arg(QDir::toNativeSeparators(path), file.errorString());
		return false;
	}

	QJsonParseError parseError;
	const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
	if (parseError.error != QJsonParseError::NoError)
	{
		if (error)
			*error = QObject::tr("Invalid library file:\n%1\n\n%2").arg(QDir::toNativeSeparators(path), parseError.errorString());
		return false;
	}
	if (!document.isObject())
	{
		if (error)
			*error = QObject::tr("Invalid library file:\n%1\n\nThe top level must be a JSON object.").arg(QDir::toNativeSeparators(path));
		return false;
	}
	if (!requiredArrayField.isEmpty() && !document.object().value(requiredArrayField).isArray())
	{
		if (error)
			*error = QObject::tr("Invalid library file:\n%1\n\nThe '%2' field is missing or is not an array.")
				.arg(QDir::toNativeSeparators(path), requiredArrayField);
		return false;
	}
	return true;
}

[[nodiscard]] std::optional<QString> validatedRoot(const QString& root, QString* error)
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

	if (!validateJsonObject(normalized + "/catalog.json", {}, error)
		|| !validateJsonObject(normalized + "/labels.json", QStringLiteral("labels"), error))
		return std::nullopt;

	return normalized;
}

} // namespace

class LibraryState
{
public:
	explicit LibraryState(QString root)
		: rootFolder(std::move(root)), metadataStore(rootFolder), catalog(rootFolder, metadataStore)
	{
	}

private:
	friend class Library;

	const QString rootFolder;
	MetadataStore metadataStore;
	Catalog catalog;
};

std::optional<Library> Library::load(const QString& root, QString* error)
{
	std::optional<QString> normalized = validatedRoot(root, error);
	if (!normalized)
		return std::nullopt;
	return Library(std::move(*normalized));
}

Library::Library(QString root)
	: m_state(std::make_unique<LibraryState>(std::move(root)))
{
}

Library::Library(Library&&) noexcept = default;
Library::~Library() = default;

bool Library::setRoot(const QString& root, QString* error)
{
	std::optional<QString> normalized = validatedRoot(root, error);
	if (!normalized)
		return false;

	auto replacement = std::make_unique<LibraryState>(std::move(*normalized));
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
