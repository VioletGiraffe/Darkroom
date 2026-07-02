#include "Core/VideoId.h"

#include <QFileInfo>

VideoId VideoId::fromFile(const QString& path)
{
	const QFileInfo info{ path };
	if (!info.isFile())
		return {};

	VideoId id;
	id._name = info.fileName();
	id._size = info.size();
	return id;
}

VideoId VideoId::fromNameAndSize(const QString& name, qint64 size)
{
	VideoId id;
	id._name = name;
	id._size = size;
	return id;
}

QString VideoId::key() const
{
	return QString::number(_size) + ':' + _name.toLower();
}

size_t qHash(const VideoId& id, size_t seed)
{
	return qHash(id.key(), seed);
}
