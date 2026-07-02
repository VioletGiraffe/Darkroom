#include "Core/MediaId.h"

#include <QFileInfo>

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
	return QString::number(_size) + ':' + _name.toLower();
}

size_t qHash(const MediaId& id, size_t seed)
{
	return qHash(id.key(), seed);
}
