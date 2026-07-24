#include "Core/MediaId.h"

#include "hash/wheathash.hpp"

#include <QFileInfo>
#include <QStringBuilder>

namespace {
	// key() (hence qHash, hence the persisted catalog key) and operator== must fold case identically, or QHash's
	// "a == b implies qHash(a) == qHash(b)" breaks and equal ids miss on lookup - hence one shared fold rather than
	// two spellings of "ignore case": toLower() is lowercase mapping, QString::compare's Qt::CaseInsensitive is case
	// folding, and the two are not guaranteed to agree on every input. toLower() is the one that stays, because
	// key() is persisted - a different fold would orphan every existing record whose name it moves.
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
	// Size first: it settles almost every comparison without folding anything.
	return _size == other._size && nameForMatching(_name) == nameForMatching(other._name);
}
