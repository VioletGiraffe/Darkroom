#include "ImageDecoding/ImageDecoding.h"
#include "ImageEncoding/ImageEncoding.h"
#include "ImageMetadata/Exif.h"
#include "JpegTestData.h"

#include "3rdparty/catch2/catch.hpp"

#include <QByteArray>
#include <QFile>
#include <QImage>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <optional>

using namespace ImageEncoding;

namespace {

[[nodiscard]] QByteArray readFile(const QString& path)
{
	QFile file{ path };
	return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

[[nodiscard]] QImage grayImage()
{
	QImage image{ 40, 30, QImage::Format_RGB32 };
	image.fill(Qt::gray);
	return image;
}

} // namespace

TEST_CASE("ImageEncoding: JPEG and PNG are writable; JPEG is lossy and takes EXIF", "[imageencoding]")
{
	const QStringList types = writableMimeTypes();
	REQUIRE(types.contains("image/jpeg"));
	REQUIRE(types.contains("image/png"));
	REQUIRE(isLossy("image/jpeg"));
	REQUIRE_FALSE(isLossy("image/png"));
	REQUIRE(takesExif("image/jpeg"));
	REQUIRE_FALSE(takesExif("image/png"));
}

TEST_CASE("ImageEncoding: a JPEG carries the source's EXIF, fitted to the image; a PNG none", "[imageencoding]")
{
	QTemporaryDir dir;
	REQUIRE(dir.isValid());
	const QByteArray source = QByteArray{ "\xFF\xD8" } + exifSegment(exifTiff(ByteOrder::Little, { { OrientationTag, 6 } })) + QByteArray{ "\xFF\xD9" };
	const QImage image = grayImage();

	const QString jpegPath = dir.filePath("out.jpg");
	REQUIRE(writeImage(image, jpegPath, "image/jpeg", DefaultQuality, source).isEmpty());
	const QByteArray jpeg = readFile(jpegPath);
	const std::optional<QByteArrayView> exif = ImageMetadata::findExifApp1(jpeg);
	REQUIRE(exif);
	REQUIRE(exif->toByteArray() == ImageMetadata::exifApp1ForOutput(source, image.size()));
	REQUIRE(ImageDecoding::decode(jpeg).image.size() == image.size());

	const QString pngPath = dir.filePath("out.png");
	REQUIRE(writeImage(image, pngPath, "image/png", DefaultQuality, source).isEmpty());
	REQUIRE_FALSE(readFile(pngPath).contains("Exif"));
}

TEST_CASE("ImageEncoding: a JPEG of a transparent image is white", "[imageencoding]")
{
	QTemporaryDir dir;
	REQUIRE(dir.isValid());
	QImage transparent{ 16, 16, QImage::Format_ARGB32_Premultiplied };
	transparent.fill(Qt::transparent);

	const QString path = dir.filePath("out.jpg");
	REQUIRE(writeImage(transparent, path, "image/jpeg", 100, {}).isEmpty());
	const QImage written = ImageDecoding::decode(readFile(path)).image;
	REQUIRE(written.size() == transparent.size());
	REQUIRE(qGray(written.pixel(8, 8)) >= 250);
}

TEST_CASE("ImageEncoding: a failed write leaves the target as it was", "[imageencoding]")
{
	QTemporaryDir dir;
	REQUIRE(dir.isValid());
	const QString path = dir.filePath("out.png");
	{
		QFile existing{ path };
		REQUIRE(existing.open(QIODevice::WriteOnly));
		existing.write("old");
	}

	REQUIRE_FALSE(writeImage(QImage{}, path, "image/png", DefaultQuality, {}).isEmpty());
	REQUIRE(readFile(path) == "old");
	REQUIRE_FALSE(writeImage(grayImage(), dir.filePath("missing/out.png"), "image/png", DefaultQuality, {}).isEmpty());
}
