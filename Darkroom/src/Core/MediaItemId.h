#pragma once

#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QString>
#include <QStringView>
RESTORE_COMPILER_WARNINGS

#include <cstdint>

// Stable media-item identity, independent of the file's name, size, content and location. Catalog mints a random
// non-None value at registration. Persisted as 16 lowercase hex digits.
enum class MediaItemId : uint64_t
{
	None = 0,
};

[[nodiscard]] inline QString toString(MediaItemId id)
{
	return QString::number(static_cast<qulonglong>(id), 16).rightJustified(16, u'0');
}

// None unless s is 16 hex digits.
[[nodiscard]] inline MediaItemId mediaItemIdFromString(QStringView s)
{
	if (s.size() != 16)
		return MediaItemId::None;
	bool ok = false;
	const qulonglong value = s.toULongLong(&ok, 16);
	return ok ? static_cast<MediaItemId>(value) : MediaItemId::None;
}
