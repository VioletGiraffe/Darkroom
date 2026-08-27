#include "Core/MediaId.h"

#include "compiler/compiler_warnings_control.h"
#include "hash/wheathash.hpp"

DISABLE_COMPILER_WARNINGS
#include <QFileInfo>
#include <QStringBuilder>
RESTORE_COMPILER_WARNINGS

namespace {
	// Equality and the persisted hash key must share this exact fold. Switching from toLower() would also orphan
	// records whose canonical names change.
	QString nameForMatching(const QString& name) { return name.toLower(); }
}

MediaId MediaId::fromFile(const QString& path)
{
	const QFileInfo info{ path };
	if (!info.isFile())
		return {};

	MediaId id;
	id._name = info.fileName();
	id._size = info.size();
	return id;
}

MediaId MediaId::fromNameAndSize(const QString& name, qint64 size)
{
	MediaId id;
	id._name = name;
	id._size = size;
	return id;
}

QString MediaId::key() const
{
	return QString::number(_size) % ':' % nameForMatching(_name);
}

uint64_t MediaId::hash() const
{
	const QString k = key();
	return ::wheathash64(k.data(), k.size() * sizeof(QChar));
}

bool MediaId::operator==(const MediaId& other) const
{
	return _size == other._size && nameForMatching(_name) == nameForMatching(other._name);
}
