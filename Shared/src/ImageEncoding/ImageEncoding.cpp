#include "ImageEncoding/ImageEncoding.h"
#include "ImageMetadata/Exif.h"
#include "assert/advanced_assert.h"

DISABLE_COMPILER_WARNINGS
#include <QBuffer>
#include <QByteArray>
#include <QColorSpace>
#include <QImage>
#include <QImageWriter>
#include <QList>
#include <QMimeDatabase>
#include <QMimeType>
#include <QObject>
#include <QPainter>
#include <QSaveFile>
RESTORE_COMPILER_WARNINGS

#include <algorithm>
#include <array>

namespace ImageEncoding {

namespace {

constexpr const char* JpegMimeType = "image/jpeg";

struct Format
{
	const char* mimeType;
	bool lossy;
};

constexpr std::array Formats{
	Format{ "image/avif", true },
	Format{ "image/bmp", false },
	Format{ "image/heic", true },
	Format{ "image/heif", true },
	Format{ JpegMimeType, true },
	Format{ "image/jxl", true },
	Format{ "image/png", false },
	Format{ "image/tiff", false },
	Format{ "image/webp", true },
};

} // namespace

QStringList writableMimeTypes()
{
	const QList<QByteArray> supportedTypes = QImageWriter::supportedMimeTypes();
	QStringList writableTypes;
	for (const Format& format : Formats)
	{
		if (supportedTypes.contains(format.mimeType))
			writableTypes.push_back(QString::fromLatin1(format.mimeType));
	}
	return writableTypes;
}

QStringList offeredMimeTypes()
{
	QStringList types = writableMimeTypes();
	if (types.contains(QLatin1String("image/heic")))
		types.removeOne(QStringLiteral("image/heif"));
	return types;
}

QString writableMimeTypeOf(const QMimeType& type)
{
	const QMimeDatabase mimeDatabase;
	const QStringList writableTypes = writableMimeTypes();
	const auto listed = std::ranges::find_if(writableTypes, [&](const QString& name) { return mimeDatabase.mimeTypeForName(name) == type; });
	return listed != writableTypes.end() ? *listed : QString{};
}

bool isLossy(const QString& mimeType)
{
	const auto format = std::ranges::find_if(Formats, [&](const Format& listed) { return mimeType == QLatin1String(listed.mimeType); });
	assert_and_return_r(format != Formats.end(), false);
	return format->lossy;
}

bool takesExif(const QString& mimeType)
{
	return mimeType == QLatin1String(JpegMimeType);
}

QString writeImage(const QImage& image, const QString& targetPath, const QString& mimeType, int quality, QByteArrayView sourceFile)
{
	const bool jpeg = mimeType == QLatin1String(JpegMimeType);
	QImage written = image;
	if (jpeg && image.hasAlphaChannel())
	{
		written = QImage{ image.size(), QImage::Format_RGB32 };
		if (written.isNull())
			return QObject::tr("Not enough memory to save a %1x%2 image").arg(image.width()).arg(image.height());
		written.setColorSpace(image.colorSpace());
		written.fill(Qt::white);
		QPainter{ &written }.drawImage(0, 0, image);
	}

	QBuffer encoded;
	encoded.open(QIODevice::WriteOnly);
	QImageWriter writer{ &encoded, QImageWriter::imageFormatsForMimeType(mimeType.toLatin1()).value(0) };
	if (isLossy(mimeType))
		writer.setQuality(quality);
	if (!writer.write(written))
		return writer.errorString();

	QByteArray bytes = encoded.data();
	if (jpeg && !sourceFile.empty())
		bytes = ImageMetadata::jpegWithExifApp1(bytes, ImageMetadata::exifApp1ForOutput(sourceFile, written.size()));

	QSaveFile target{ targetPath };
	if (!target.open(QIODevice::WriteOnly))
		return target.errorString();
	if (target.write(bytes) != bytes.size())
		return target.errorString();
	return target.commit() ? QString{} : target.errorString();
}

} // namespace ImageEncoding
