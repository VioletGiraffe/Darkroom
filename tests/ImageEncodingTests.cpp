#include "ImageDecoding/ColorManagement.h"
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

#include <turbojpeg.h>

#include <memory>
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

// TJSAMP_*; -1 when jpeg is not one libjpeg-turbo reads
[[nodiscard]] int jpegSubsampling(const QByteArray& jpeg)
{
	const std::unique_ptr<void, decltype(&tj3Destroy)> handle{ tj3Init(TJINIT_DECOMPRESS), &tj3Destroy };
	if (tj3DecompressHeader(handle.get(), reinterpret_cast<const unsigned char*>(jpeg.constData()), static_cast<size_t>(jpeg.size())) != 0)
		return -1;
	return tj3Get(handle.get(), TJPARAM_SUBSAMP);
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
	REQUIRE(writeImage(image, {}, jpegPath, { .mimeType = "image/jpeg" }, source).isEmpty());
	const QByteArray jpeg = readFile(jpegPath);
	const std::optional<QByteArrayView> exif = ImageMetadata::findExifApp1(jpeg);
	REQUIRE(exif);
	REQUIRE(exif->toByteArray() == ImageMetadata::exifApp1ForOutput(source, image.size()));
	REQUIRE(ImageDecoding::decode(jpeg).image.size() == image.size());

	const QString pngPath = dir.filePath("out.png");
	REQUIRE(writeImage(image, {}, pngPath, { .mimeType = "image/png" }, source).isEmpty());
	REQUIRE_FALSE(readFile(pngPath).contains("Exif"));
}

TEST_CASE("ImageEncoding: a JPEG embeds the ICC profile and has the chroma subsampling asked for", "[imageencoding]")
{
	const QByteArray& profile = ImageDecoding::adobeRgbIccProfile();
	const auto encoded = [&](const QImage& image, JpegSubsampling subsampling) {
		const EncodedFile jpeg = encodeImage(image, profile, { .mimeType = "image/jpeg", .jpegSubsampling = subsampling }, {});
		REQUIRE(jpeg.error.isEmpty());
		return jpeg.bytes;
	};

	const QByteArray jpeg = encoded(grayImage(), JpegSubsampling::Chroma444);
	REQUIRE(ImageDecoding::decode(jpeg).iccProfile == profile);
	REQUIRE(jpegSubsampling(jpeg) == TJSAMP_444);
	REQUIRE(jpegSubsampling(encoded(grayImage(), JpegSubsampling::Chroma422)) == TJSAMP_422);
	REQUIRE(jpegSubsampling(encoded(grayImage(), JpegSubsampling::Chroma420)) == TJSAMP_420);
	REQUIRE(jpegSubsampling(encoded(grayImage().convertToFormat(QImage::Format_Grayscale8), JpegSubsampling::Chroma420)) == TJSAMP_GRAY);
}

TEST_CASE("ImageEncoding: a JPEG of a transparent image is white", "[imageencoding]")
{
	QTemporaryDir dir;
	REQUIRE(dir.isValid());
	QImage transparent{ 16, 16, QImage::Format_ARGB32_Premultiplied };
	transparent.fill(Qt::transparent);

	const QString path = dir.filePath("out.jpg");
	REQUIRE(writeImage(transparent, {}, path, { .mimeType = "image/jpeg", .quality = 100 }, {}).isEmpty());
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

	REQUIRE_FALSE(writeImage(QImage{}, {}, path, { .mimeType = "image/png" }, {}).isEmpty());
	REQUIRE(readFile(path) == "old");
	REQUIRE_FALSE(writeImage(grayImage(), {}, dir.filePath("missing/out.png"), { .mimeType = "image/png" }, {}).isEmpty());
}

TEST_CASE("ImageEncoding: writeFileAtomically runs beforeReplacing before the file takes its path", "[imageencoding]")
{
	QTemporaryDir dir;
	REQUIRE(dir.isValid());
	const QString path = dir.filePath("out.bin");
	QByteArray seenBeforeReplacing = "unset";
	REQUIRE(writeFileAtomically(path, "new", [&] { seenBeforeReplacing = readFile(path); }).isEmpty());
	REQUIRE(seenBeforeReplacing.isEmpty()); // no file there yet
	REQUIRE(readFile(path) == "new");
}
