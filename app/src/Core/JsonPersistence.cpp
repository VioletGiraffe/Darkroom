#include "Core/JsonPersistence.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QObject>
#include <QSaveFile>

JsonPersistence::ObjectReadResult JsonPersistence::readObject(const QString& path, QStringView requiredArrayField)
{
	ObjectReadResult result;
	const QFileInfo info(path);
	if (!info.exists() && !info.isSymLink())
	{
		result.success = true;
		return result;
	}

	QFile file(path);
	if (!file.open(QIODevice::ReadOnly))
	{
		result.error = QObject::tr("Could not read:\n%1\n\n%2").arg(QDir::toNativeSeparators(path), file.errorString());
		return result;
	}

	const QByteArray data = file.readAll();
	if (file.error() != QFileDevice::NoError)
	{
		result.error = QObject::tr("Could not read:\n%1\n\n%2").arg(QDir::toNativeSeparators(path), file.errorString());
		return result;
	}

	QJsonParseError parseError;
	const QJsonDocument document = QJsonDocument::fromJson(data, &parseError);
	if (parseError.error != QJsonParseError::NoError)
	{
		result.error = QObject::tr("Invalid JSON file:\n%1\n\nError at byte %2: %3")
			.arg(QDir::toNativeSeparators(path)).arg(parseError.offset).arg(parseError.errorString());
		return result;
	}
	if (!document.isObject())
	{
		result.error = QObject::tr("Invalid JSON file:\n%1\n\nThe top level must be a JSON object.")
			.arg(QDir::toNativeSeparators(path));
		return result;
	}

	result.object = document.object();
	if (!requiredArrayField.isEmpty() && !result.object.value(requiredArrayField.toString()).isArray())
	{
		result.error = QObject::tr("Invalid JSON file:\n%1\n\nThe '%2' field is missing or is not an array.")
			.arg(QDir::toNativeSeparators(path), requiredArrayField.toString());
		return result;
	}

	result.success = true;
	return result;
}

QString JsonPersistence::writeObject(const QString& path, const QJsonObject& object)
{
	const QString parentFolder = QFileInfo(path).absolutePath();
	if (!QDir{}.mkpath(parentFolder))
		return QObject::tr("Could not create the folder needed to save:\n%1").arg(QDir::toNativeSeparators(parentFolder));

	QSaveFile file(path);
	if (!file.open(QIODevice::WriteOnly))
		return QObject::tr("Could not open for writing:\n%1\n\n%2").arg(QDir::toNativeSeparators(path), file.errorString());

	const QByteArray data = QJsonDocument(object).toJson(QJsonDocument::Indented);
	const qint64 written = file.write(data);
	if (written != data.size())
	{
		const QString writeError = file.errorString();
		file.cancelWriting();
		return QObject::tr("Could not write the complete file:\n%1\n\n%2")
			.arg(QDir::toNativeSeparators(path), writeError);
	}
	if (!file.commit())
		return QObject::tr("Could not finish saving:\n%1\n\n%2").arg(QDir::toNativeSeparators(path), file.errorString());
	return {};
}
