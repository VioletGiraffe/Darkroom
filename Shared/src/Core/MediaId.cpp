#include "Core/MediaId.h"

#include "compiler/compiler_warnings_control.h"
#include "hash/rapidhash.hpp"

DISABLE_COMPILER_WARNINGS
#include <QFileInfo>
#include <QStringBuilder>
RESTORE_COMPILER_WARNINGS

#include <utility>

namespace {
	// Equality and the persisted key share this exact fold. Switching from toLower() would also orphan
	// records whose canonical names change.
	QString nameForMatching(const QString& name) { return name.toLower(); }
}

MediaId::MediaId(QString name, qint64 size)
	: _name{ std::move(name) }, _matchName{ nameForMatching(_name) }, _size{ size }
{
}

MediaId MediaId::fromFile(const QString& path)
{
	const QFileInfo info{ path };
	if (!info.isFile())
		return {};

	return MediaId{ info.fileName(), info.size() };
}

MediaId MediaId::fromNameAndSize(const QString& name, qint64 size)
{
	return MediaId{ name, size };
}

QString MediaId::key() const
{
	return QString::number(_size) % ':' % _matchName;
}

uint64_t MediaId::hash() const
{
	return ::rapidhash64(_matchName.constData(), _matchName.size() * sizeof(QChar), static_cast<uint64_t>(_size));
}

bool MediaId::operator==(const MediaId& other) const
{
	return _size == other._size && _matchName == other._matchName;
}
