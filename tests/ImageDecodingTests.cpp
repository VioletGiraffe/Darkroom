#include "ImageDecoding/ImageDecoding.h"
#include "ImageDecoding/TurboJpegDecoder.h"
#include "JpegTestData.h"

#include "3rdparty/catch2/catch.hpp"

#include <QBuffer>
#include <QByteArray>
#include <QFile>
#include <QImage>
#include <QPoint>
#include <QSize>
#include <QTemporaryDir>

DISABLE_COMPILER_WARNINGS
#include <turbojpeg.h>
RESTORE_COMPILER_WARNINGS

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <vector>

using namespace ImageDecoding;

namespace {

enum class JpegKind { Rgb, Gray, Cmyk, Ycck, Rgb12Bit };

// Each fills a half of every test JPEG; the halves meet on a block boundary, so both stay flat after compression
constexpr QRgb LeftColor = qRgb(200, 30, 40);
constexpr QRgb RightColor = qRgb(20, 60, 210);

constexpr QSize JpegSize{ 64, 32 };

[[nodiscard]] int sampleOf(QRgb color, int channel, int pixelFormat)
{
	switch (pixelFormat)
	{
	case TJPF_GRAY: return qGray(color);
	case TJPF_RGB: return channel == 0 ? qRed(color) : channel == 1 ? qGreen(color) : qBlue(color);
	default: return 40 * channel; // CMYK: any ink
	}
}

[[nodiscard]] QByteArray encodeJpeg(JpegKind kind)
{
	const std::unique_ptr<void, decltype(&tj3Destroy)> handle{ tj3Init(TJINIT_COMPRESS), &tj3Destroy };
	REQUIRE(handle != nullptr);

	const bool gray = kind == JpegKind::Gray;
	const bool cmyk = kind == JpegKind::Cmyk || kind == JpegKind::Ycck;
	REQUIRE(tj3Set(handle.get(), TJPARAM_QUALITY, 95) == 0);
	REQUIRE(tj3Set(handle.get(), TJPARAM_SUBSAMP, gray ? TJSAMP_GRAY : TJSAMP_444) == 0);
	if (cmyk)
		REQUIRE(tj3Set(handle.get(), TJPARAM_COLORSPACE, kind == JpegKind::Cmyk ? TJCS_CMYK : TJCS_YCCK) == 0);

	const int pixelFormat = gray ? TJPF_GRAY : cmyk ? TJPF_CMYK : TJPF_RGB;
	// scale: 16 maps 8-bit levels onto 12-bit samples
	const auto samples = [&]<typename Sample>(Sample scale) {
		std::vector<Sample> values;
		for (int y = 0; y < JpegSize.height(); ++y)
		{
			for (int x = 0; x < JpegSize.width(); ++x)
			{
				for (int c = 0; c < tjPixelSize[pixelFormat]; ++c)
					values.push_back(static_cast<Sample>(sampleOf(x < JpegSize.width() / 2 ? LeftColor : RightColor, c, pixelFormat) * scale));
			}
		}
		return values;
	};

	unsigned char* jpegBuffer = nullptr;
	size_t jpegSize = 0;
	const int result = kind == JpegKind::Rgb12Bit
		? tj3Compress12(handle.get(), samples(short{ 16 }).data(), JpegSize.width(), 0, JpegSize.height(), pixelFormat, &jpegBuffer, &jpegSize)
		: tj3Compress8(handle.get(), samples(static_cast<unsigned char>(1)).data(), JpegSize.width(), 0, JpegSize.height(), pixelFormat, &jpegBuffer, &jpegSize);
	const std::unique_ptr<unsigned char, decltype(&tj3Free)> jpegOwner{ jpegBuffer, &tj3Free };
	REQUIRE(result == 0);
	return QByteArray{ reinterpret_cast<const char*>(jpegBuffer), static_cast<qsizetype>(jpegSize) };
}

[[nodiscard]] QByteArray withExifOrientation(const QByteArray& jpeg, uint16_t orientation)
{
	return jpeg.first(2) + exifSegment(exifTiff(ByteOrder::Big, { { OrientationTag, orientation } })) + jpeg.sliced(2);
}

// Within a few levels per channel: JPEG is lossy
[[nodiscard]] bool isNear(QRgb actual, QRgb expected)
{
	constexpr int tolerance = 4;
	return std::abs(qRed(actual) - qRed(expected)) <= tolerance && std::abs(qGreen(actual) - qGreen(expected)) <= tolerance
		&& std::abs(qBlue(actual) - qBlue(expected)) <= tolerance;
}

// Sampled at the centre of each half
[[nodiscard]] bool showsBothHalves(const QImage& image)
{
	const int y = image.height() / 2;
	return isNear(image.pixel(image.width() / 4, y), LeftColor) && isNear(image.pixel(image.width() * 3 / 4, y), RightColor);
}

// Format_RGB32 requires 0xFF in every pixel's unused byte
[[nodiscard]] bool hasOpaqueFourthBytes(const QImage& image)
{
	for (int y = 0; y < image.height(); ++y)
	{
		const auto* line = reinterpret_cast<const uint32_t*>(image.constScanLine(y));
		for (int x = 0; x < image.width(); ++x)
		{
			if (line[x] >> 24 != 0xFF)
				return false;
		}
	}
	return true;
}

} // namespace

TEST_CASE("ImageDecoding::decode: JPEGs through libjpeg-turbo", "[imagedecoding]")
{
	SECTION("colour")
	{
		const QByteArray jpeg = encodeJpeg(JpegKind::Rgb);
		REQUIRE(decoderFor("C:/p/photo.jpg", jpeg.first(SignatureSize)) == Decoder::TurboJpeg);

		const DecodedImage decoded = decode(jpeg);
		REQUIRE(decoded.error.isEmpty());
		REQUIRE(decoded.format == "jpeg");
		REQUIRE(decoded.orientation == QImageIOHandler::TransformationNone);
		REQUIRE(decoded.image.size() == JpegSize);
		REQUIRE(decoded.image.format() == QImage::Format_RGB32);
		REQUIRE(hasOpaqueFourthBytes(decoded.image));
		REQUIRE(showsBothHalves(decoded.image));
	}

	SECTION("grayscale")
	{
		const DecodedImage decoded = decode(encodeJpeg(JpegKind::Gray));
		REQUIRE(decoded.error.isEmpty());
		REQUIRE(decoded.image.size() == JpegSize);
		REQUIRE(decoded.image.format() == QImage::Format_Grayscale8);
		const int y = JpegSize.height() / 2;
		REQUIRE(std::abs(qGray(decoded.image.pixel(JpegSize.width() / 4, y)) - qGray(LeftColor)) <= 4);
		REQUIRE(std::abs(qGray(decoded.image.pixel(JpegSize.width() * 3 / 4, y)) - qGray(RightColor)) <= 4);
	}
}

TEST_CASE("decodeJpeg: CMYK, YCCK and 12-bit JPEGs are left to Qt", "[imagedecoding]")
{
	for (const JpegKind kind : { JpegKind::Cmyk, JpegKind::Ycck, JpegKind::Rgb12Bit })
	{
		CAPTURE(static_cast<int>(kind));
		REQUIRE_FALSE(decodeJpeg(encodeJpeg(kind)).has_value());
	}
}

TEST_CASE("ImageDecoding::decode: the EXIF orientation comes with the image as stored", "[imagedecoding]")
{
	const DecodedImage decoded = decode(withExifOrientation(encodeJpeg(JpegKind::Rgb), 6));
	REQUIRE(decoded.error.isEmpty());
	REQUIRE(decoded.orientation == QImageIOHandler::TransformationRotate90);
	REQUIRE(decoded.image.size() == JpegSize);
	REQUIRE(showsBothHalves(decoded.image));
}

TEST_CASE("ImageDecoding::decode: damaged JPEGs", "[imagedecoding]")
{
	const QByteArray jpeg = encodeJpeg(JpegKind::Rgb);

	SECTION("a truncated scan keeps the image, the decoded part intact")
	{
		const qsizetype scanStart = jpeg.indexOf("\xFF\xDA");
		REQUIRE(scanStart > 0);
		const DecodedImage decoded = decode(jpeg.first(scanStart + (jpeg.size() - scanStart) / 2));
		REQUIRE(decoded.error.isEmpty());
		REQUIRE(decoded.image.size() == JpegSize);
		REQUIRE(isNear(decoded.image.pixel(0, 0), LeftColor));
	}

	SECTION("a damaged header fails")
	{
		const DecodedImage decoded = decode(jpeg.first(SignatureSize) + QByteArray(64, '\x55'));
		REQUIRE(decoded.image.isNull());
		REQUIRE_FALSE(decoded.error.isEmpty());
		REQUIRE(decoded.format == "jpeg");
	}
}

TEST_CASE("ImageDecoding::decode: other formats through Qt", "[imagedecoding]")
{
	QImage source{ 8, 4, QImage::Format_RGB32 };
	source.fill(LeftColor);
	source.setPixel(5, 2, RightColor);
	QByteArray png;
	QBuffer buffer{ &png };
	REQUIRE(buffer.open(QIODevice::WriteOnly));
	REQUIRE(source.save(&buffer, "PNG"));
	REQUIRE(decoderFor("C:/p/image.png", png.first(SignatureSize)) == Decoder::Qt);

	const DecodedImage decoded = decode(png);
	REQUIRE(decoded.error.isEmpty());
	REQUIRE(decoded.format == "png");
	REQUIRE(decoded.image.convertToFormat(QImage::Format_RGB32) == source);

	const DecodedImage garbage = decode(QByteArray{ "not an image" });
	REQUIRE(garbage.image.isNull());
	REQUIRE_FALSE(garbage.error.isEmpty());
	REQUIRE(garbage.format.isEmpty());
}

TEST_CASE("ImageDecoding::decode: an EncodedImage's format and orientation replace the ones in its bytes", "[imagedecoding]")
{
	const QByteArray jpeg = withExifOrientation(encodeJpeg(JpegKind::Rgb), 6);

	const DecodedImage plain = decode(EncodedImage{ .bytes = jpeg });
	REQUIRE(plain.format == "jpeg");
	REQUIRE(plain.orientation == QImageIOHandler::TransformationRotate90);

	const DecodedImage preview = decode(EncodedImage{ .bytes = jpeg, .format = "nef", .orientation = QImageIOHandler::TransformationRotate180 });
	REQUIRE_FALSE(preview.image.isNull());
	REQUIRE(preview.format == "nef");
	REQUIRE(preview.orientation == QImageIOHandler::TransformationRotate180);

	const DecodedImage failed = decode(EncodedImage{ .error = "read failed", .format = "nef" });
	REQUIRE(failed.image.isNull());
	REQUIRE(failed.error == "read failed");
	REQUIRE(failed.format == "nef");
}

TEST_CASE("ImageDecoding::decoderFor: camera raws by extension, other files by content", "[imagedecoding]")
{
	const QByteArray jpegSignature{ "\xFF\xD8\xFF" };
	const QByteArray pngSignature{ "\x89PN" };

	REQUIRE(decoderFor("C:/p/raw.NEF", pngSignature) == Decoder::LibRaw);
	REQUIRE(decoderFor("C:/p/raw.dng", jpegSignature) == Decoder::LibRaw);
	REQUIRE(decoderFor("C:/p/misnamed.png", jpegSignature) == Decoder::TurboJpeg);
	REQUIRE(decoderFor("C:/p/misnamed.jpg", pngSignature) == Decoder::Qt);
	REQUIRE(decoderFor("C:/raws.nef/photo", jpegSignature) == Decoder::TurboJpeg);
}

TEST_CASE("ImageDecoding::readEncodedImage: files are read whole, camera raws through LibRaw", "[imagedecoding]")
{
	QTemporaryDir dir;
	REQUIRE(dir.isValid());
	const auto writeFile = [&](const QString& name, const QByteArray& contents) {
		const QString path = dir.filePath(name);
		QFile file{ path };
		REQUIRE(file.open(QIODevice::WriteOnly));
		REQUIRE(file.write(contents) == contents.size());
		return path;
	};

	const QByteArray jpeg = encodeJpeg(JpegKind::Rgb);
	const EncodedImage photo = readEncodedImage(writeFile("photo.jpg", jpeg), Intent::Thumbnail);
	REQUIRE(photo.error.isEmpty());
	REQUIRE(photo.bytes == jpeg);
	REQUIRE(photo.format.isEmpty());
	REQUIRE_FALSE(photo.orientation.has_value());

	// Larger than buffered_reader's window
	QByteArray large(300 * 1024, Qt::Uninitialized);
	for (qsizetype i = 0; i < large.size(); ++i)
		large[i] = static_cast<char>(i * 7 + i / 251);
	REQUIRE(readEncodedImage(writeFile("large.bin", large), Intent::Full).bytes == large);

	const EncodedImage missing = readEncodedImage(dir.filePath("missing.jpg"), Intent::Thumbnail);
	REQUIRE_FALSE(missing.error.isEmpty());
	REQUIRE(missing.bytes.isEmpty());

	// A JPEG is no raw to LibRaw, whatever its extension
	const EncodedImage notRaw = readEncodedImage(writeFile("photo.nef", jpeg), Intent::Thumbnail);
	REQUIRE_FALSE(notRaw.error.isEmpty());
	REQUIRE(notRaw.bytes.isEmpty());
	REQUIRE(notRaw.format == "nef");
}

TEST_CASE("ImageDecoding::applyOrientation: every transformation as EXIF defines it", "[imagedecoding]")
{
	constexpr int W = 3;
	constexpr int H = 2;
	QImage source{ W, H, QImage::Format_RGB32 };
	for (int y = 0; y < H; ++y)
	{
		for (int x = 0; x < W; ++x)
			source.setPixel(x, y, qRgb(x * 80, y * 80, 0));
	}

	struct Expected
	{
		QImageIOHandler::Transformations orientation;
		bool turned;
		QPoint (*sourceOf)(int x, int y); // the source pixel shown at (x, y)
	};
	const Expected expected[]{
		{ QImageIOHandler::TransformationNone, false, [](int x, int y) { return QPoint{ x, y }; } },
		{ QImageIOHandler::TransformationMirror, false, [](int x, int y) { return QPoint{ W - 1 - x, y }; } },
		{ QImageIOHandler::TransformationFlip, false, [](int x, int y) { return QPoint{ x, H - 1 - y }; } },
		{ QImageIOHandler::TransformationRotate180, false, [](int x, int y) { return QPoint{ W - 1 - x, H - 1 - y }; } },
		{ QImageIOHandler::TransformationRotate90, true, [](int x, int y) { return QPoint{ y, H - 1 - x }; } },          // clockwise
		{ QImageIOHandler::TransformationFlipAndRotate90, true, [](int x, int y) { return QPoint{ y, x }; } },           // transpose
		{ QImageIOHandler::TransformationMirrorAndRotate90, true, [](int x, int y) { return QPoint{ W - 1 - y, H - 1 - x }; } }, // transverse
		{ QImageIOHandler::TransformationRotate270, true, [](int x, int y) { return QPoint{ W - 1 - y, x }; } },         // counterclockwise
	};

	for (const Expected& e : expected)
	{
		CAPTURE(e.orientation.toInt());
		QImage image = source;
		applyOrientation(image, e.orientation);
		REQUIRE(image.size() == (e.turned ? source.size().transposed() : source.size()));
		for (int y = 0; y < image.height(); ++y)
		{
			for (int x = 0; x < image.width(); ++x)
			{
				CAPTURE(x, y);
				REQUIRE(image.pixel(x, y) == source.pixel(e.sourceOf(x, y)));
			}
		}
	}
}
