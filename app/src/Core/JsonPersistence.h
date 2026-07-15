#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringView>

namespace JsonPersistence {

struct ObjectReadResult
{
	bool success = false;
	QJsonObject object;
	QString error;
};

// Missing is a successful empty result. An existing file must be readable JSON with an object root; when
// requiredArrayField is non-empty, that field must also exist as an array.
[[nodiscard]] ObjectReadResult readObject(const QString& path, QStringView requiredArrayField = {});

// Atomically replaces path with an indented JSON object. Empty return means success; otherwise the returned
// text is user-presentable and the previous file remains in place whenever QSaveFile can preserve it.
[[nodiscard]] QString writeObject(const QString& path, const QJsonObject& object);

} // namespace JsonPersistence
