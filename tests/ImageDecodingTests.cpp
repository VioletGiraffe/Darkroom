#include "ImageDecoding/ColorManagement.h"
#include "ImageDecoding/ImageDecoding.h"
#include "ImageDecoding/TurboJpegDecoder.h"
#include "JpegTestData.h"

#include "3rdparty/catch2/catch.hpp"

#include <QBuffer>
#include <QByteArray>
#include <QColorSpace>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QPoint>
#include <QSize>
#include <QTemporaryDir>

DISABLE_COMPILER_WARNINGS
#include <turbojpeg.h>
RESTORE_COMPILER_WARNINGS

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
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

[[nodiscard]] QByteArray encodeJpeg(JpegKind kind, QByteArray iccProfile = {})
{
	const std::unique_ptr<void, decltype(&tj3Destroy)> handle{ tj3Init(TJINIT_COMPRESS), &tj3Destroy };
	REQUIRE(handle != nullptr);
	if (!iccProfile.isEmpty())
		REQUIRE(tj3SetICCProfile(handle.get(), reinterpret_cast<unsigned char*>(iccProfile.data()), static_cast<size_t>(iccProfile.size())) == 0);

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

TEST_CASE("ImageDecoding::decode: the color profile comes with the image", "[imagedecoding]")
{
	const QByteArray displayP3 = QColorSpace{ QColorSpace::DisplayP3 }.iccProfile();
	const QByteArray adobeRgbExif = exifSegment(exifTiffWithColorSpace(ByteOrder::Big, 0xFFFF, "R03"));
	const auto withSegment = [](const QByteArray& jpeg, const QByteArray& segment) { return jpeg.first(2) + segment + jpeg.sliced(2); };

	SECTION("embedded in a JPEG")
	{
		const DecodedImage decoded = decode(encodeJpeg(JpegKind::Rgb, displayP3));
		REQUIRE(decoded.error.isEmpty());
		REQUIRE(decoded.iccProfile == displayP3);
		REQUIRE(showsBothHalves(decoded.image)); // unconverted
	}

	SECTION("Adobe RGB declared in a JPEG's EXIF, a profile it embeds taking precedence")
	{
		REQUIRE(decode(withSegment(encodeJpeg(JpegKind::Rgb), adobeRgbExif)).iccProfile == adobeRgbIccProfile());
		REQUIRE(decode(withSegment(encodeJpeg(JpegKind::Rgb, displayP3), adobeRgbExif)).iccProfile == displayP3);
	}

	SECTION("Adobe RGB declared for a camera raw's preview, a profile it embeds taking precedence")
	{
		REQUIRE(decode(EncodedImage{ .bytes = encodeJpeg(JpegKind::Rgb), .adobeRgb = true }).iccProfile == adobeRgbIccProfile());
		REQUIRE(decode(EncodedImage{ .bytes = encodeJpeg(JpegKind::Rgb, displayP3), .adobeRgb = true }).iccProfile == displayP3);
		REQUIRE(decode(EncodedImage{ .error = "read failed", .adobeRgb = true }).iccProfile.isEmpty());
	}

	SECTION("through Qt")
	{
		QImage source{ 8, 4, QImage::Format_RGB32 };
		source.fill(LeftColor);
		source.setColorSpace(QColorSpace::DisplayP3);
		QByteArray png;
		QBuffer buffer{ &png };
		REQUIRE(buffer.open(QIODevice::WriteOnly));
		REQUIRE(source.save(&buffer, "PNG"));

		const DecodedImage decoded = decode(png);
		REQUIRE(decoded.error.isEmpty());
		REQUIRE(QColorSpace::fromIccProfile(decoded.iccProfile) == QColorSpace{ QColorSpace::DisplayP3 });
		REQUIRE_FALSE(decoded.animated);
	}

	SECTION("none")
	{
		REQUIRE(decode(encodeJpeg(JpegKind::Rgb)).iccProfile.isEmpty());
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
		const DecodedImage decoded = decode(jpeg.first(3) + QByteArray(64, '\x55')); // SOI, then a marker's FF
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

TEST_CASE("decodeJpeg: DCT scaling rounds each side up", "[imagedecoding]")
{
	const QByteArray jpeg = encodeJpeg(JpegKind::Rgb);
	for (const int scaleEighths : { 1, 2, 3, 4, 5, 6, 7, 8 })
	{
		CAPTURE(scaleEighths);
		const std::optional<DecodedImage> decoded = decodeJpeg(jpeg, scaleEighths);
		REQUIRE(decoded.has_value());
		REQUIRE(decoded->error.isEmpty());
		REQUIRE(decoded->image.size() == QSize{ (JpegSize.width() * scaleEighths + 7) / 8, (JpegSize.height() * scaleEighths + 7) / 8 });
		REQUIRE(hasOpaqueFourthBytes(decoded->image));
		REQUIRE(showsBothHalves(decoded->image));
	}

	REQUIRE(readJpegStoredSize(jpeg) == JpegSize);
	REQUIRE(readJpegStoredSize(encodeJpeg(JpegKind::Cmyk)).isEmpty());
	REQUIRE(readJpegStoredSize(jpeg.first(3) + QByteArray(64, '\x55')).isEmpty());
}

TEST_CASE("ImageDecoding::planJpegReduction: the smallest scale covering the box", "[imagedecoding]")
{
	const QByteArray jpeg = encodeJpeg(JpegKind::Rgb); // 64x32

	SECTION("the fit scale rounded up to eighths")
	{
		const std::optional<JpegReduction> quarter = planJpegReduction(EncodedImage{ .bytes = jpeg }, QSize{ 100, 8 });
		REQUIRE(quarter.has_value());
		REQUIRE(quarter->uprightSize == JpegSize);
		REQUIRE(quarter->scaleEighths == 2);

		const std::optional<JpegReduction> roundedUp = planJpegReduction(EncodedImage{ .bytes = jpeg }, QSize{ 17, 100 });
		REQUIRE(roundedUp.has_value());
		REQUIRE(roundedUp->scaleEighths == 3);
	}

	SECTION("never below 1/8 or above 1")
	{
		REQUIRE(planJpegReduction(EncodedImage{ .bytes = jpeg }, QSize{ 1, 1 })->scaleEighths == 1);
		REQUIRE(planJpegReduction(EncodedImage{ .bytes = jpeg }, QSize{ 1000, 1000 })->scaleEighths == 8);
	}

	SECTION("the box is upright: EXIF's orientation, or the EncodedImage's in its place")
	{
		const QByteArray rotated = withExifOrientation(jpeg, 6);
		const QSize portraitBox = JpegSize.transposed();
		const std::optional<JpegReduction> fromExif = planJpegReduction(EncodedImage{ .bytes = rotated }, portraitBox);
		REQUIRE(fromExif.has_value());
		REQUIRE(fromExif->uprightSize == JpegSize.transposed());
		REQUIRE(fromExif->scaleEighths == 8);

		const std::optional<JpegReduction> fromRaw = planJpegReduction(EncodedImage{ .bytes = rotated, .orientation = QImageIOHandler::TransformationNone }, portraitBox);
		REQUIRE(fromRaw.has_value());
		REQUIRE(fromRaw->uprightSize == JpegSize);
		REQUIRE(fromRaw->scaleEighths == 4);
	}

	SECTION("empty for what decode() does not read through libjpeg-turbo")
	{
		REQUIRE_FALSE(planJpegReduction(EncodedImage{ .bytes = encodeJpeg(JpegKind::Cmyk) }, QSize{ 8, 8 }).has_value());
		REQUIRE_FALSE(planJpegReduction(EncodedImage{ .bytes = jpeg, .rawRendering = RawRendering::HalfSize }, QSize{ 8, 8 }).has_value());
		REQUIRE_FALSE(planJpegReduction(EncodedImage{ .error = "read failed" }, QSize{ 8, 8 }).has_value());
		REQUIRE_FALSE(planJpegReduction(EncodedImage{ .bytes = QByteArray{ "not an image" } }, QSize{ 8, 8 }).has_value());
		REQUIRE_FALSE(planJpegReduction(EncodedImage{ .bytes = jpeg }, QSize{}).has_value());
	}
}

TEST_CASE("ImageDecoding::decodeReduced: an EncodedImage's format and orientation apply", "[imagedecoding]")
{
	const QByteArray jpeg = withExifOrientation(encodeJpeg(JpegKind::Rgb), 6);
	const DecodedImage reduced = decodeReduced(EncodedImage{ .bytes = jpeg, .format = "nef", .orientation = QImageIOHandler::TransformationRotate180 }, 4);
	REQUIRE(reduced.error.isEmpty());
	REQUIRE(reduced.image.size() == JpegSize / 2);
	REQUIRE(reduced.format == "nef");
	REQUIRE(reduced.orientation == QImageIOHandler::TransformationRotate180);
	REQUIRE(showsBothHalves(reduced.image));

	const DecodedImage plain = decodeReduced(EncodedImage{ .bytes = jpeg }, 2);
	REQUIRE(plain.format == "jpeg");
	REQUIRE(plain.orientation == QImageIOHandler::TransformationRotate90);
	REQUIRE(plain.image.size() == JpegSize / 4);
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
	const QString photoPath = writeFile("photo.jpg", jpeg);
	for (const RawRendering rawRendering : { RawRendering::EmbeddedPreview, RawRendering::HalfSize, RawRendering::Full })
	{
		CAPTURE(static_cast<int>(rawRendering));
		const EncodedImage photo = readEncodedImage(photoPath, rawRendering);
		REQUIRE(photo.error.isEmpty());
		REQUIRE(photo.bytes == jpeg);
		REQUIRE(photo.format.isEmpty());
		REQUIRE_FALSE(photo.orientation.has_value());
		REQUIRE_FALSE(photo.rawRendering.has_value());
	}

	// Larger than buffered_reader's window
	QByteArray large(300 * 1024, Qt::Uninitialized);
	for (qsizetype i = 0; i < large.size(); ++i)
		large[i] = static_cast<char>(i * 7 + i / 251);
	REQUIRE(readEncodedImage(writeFile("large.bin", large), RawRendering::Full).bytes == large);

	const EncodedImage missing = readEncodedImage(dir.filePath("missing.jpg"), RawRendering::EmbeddedPreview);
	REQUIRE_FALSE(missing.error.isEmpty());
	REQUIRE(missing.bytes.isEmpty());

	// A JPEG is no raw to LibRaw, whatever its extension
	const QString notRawPath = writeFile("photo.nef", jpeg);
	const EncodedImage notRawPreview = readEncodedImage(notRawPath, RawRendering::EmbeddedPreview);
	REQUIRE_FALSE(notRawPreview.error.isEmpty());
	REQUIRE(notRawPreview.bytes.isEmpty());
	REQUIRE(notRawPreview.format == "nef");

	// Rendered raws are read whole and fail only in decode()
	for (const RawRendering rawRendering : { RawRendering::HalfSize, RawRendering::Full })
	{
		CAPTURE(static_cast<int>(rawRendering));
		const EncodedImage notRaw = readEncodedImage(notRawPath, rawRendering);
		REQUIRE(notRaw.error.isEmpty());
		REQUIRE(notRaw.bytes == jpeg);
		REQUIRE(notRaw.format == "nef");
		REQUIRE(notRaw.rawRendering == rawRendering);

		const DecodedImage decoded = decode(notRaw);
		REQUIRE(decoded.image.isNull());
		REQUIRE_FALSE(decoded.error.isEmpty());
		REQUIRE(decoded.format == "nef");
	}

	// Only a raw LibRaw parses has a preview to hand over first; everything else reads as without the handler
	bool previewHandled = false;
	const RawPreviewHandler abandon = [&](EncodedImage, QSize) {
		previewHandled = true;
		return false;
	};
	for (const RawRendering rawRendering : { RawRendering::EmbeddedPreview, RawRendering::HalfSize, RawRendering::Full })
	{
		CAPTURE(static_cast<int>(rawRendering));
		REQUIRE(readEncodedImage(photoPath, rawRendering, abandon).bytes == jpeg);
		const EncodedImage notRaw = readEncodedImage(notRawPath, rawRendering, abandon);
		REQUIRE(notRaw.format == "nef");
		REQUIRE(notRaw.bytes == (rawRendering == RawRendering::EmbeddedPreview ? QByteArray{} : jpeg));
	}
	REQUIRE_FALSE(previewHandled);
}

// Hidden: renders every camera raw in the folder named by DARKROOM_RAW_SAMPLES in each way, reporting size and time
TEST_CASE("ImageDecoding: camera raw samples", "[.][rawsamples]")
{
	const QString folder = qEnvironmentVariable("DARKROOM_RAW_SAMPLES");
	REQUIRE_FALSE(folder.isEmpty());

	const QFileInfoList files = QDir{ folder }.entryInfoList(QDir::Files, QDir::Name);
	for (const QFileInfo& file : files)
	{
		if (!rawFileSuffixes().contains(file.suffix().toLower()))
			continue;

		for (const RawRendering rawRendering : { RawRendering::EmbeddedPreview, RawRendering::HalfSize, RawRendering::Full })
		{
			QElapsedTimer timer;
			timer.start();
			const EncodedImage encoded = readEncodedImage(file.filePath(), rawRendering);
			const qint64 readMs = timer.restart();
			// For tuning LibRaw's stage weights: each reported fraction with the time it came at
			std::vector<double> fractions;
			QString progressLog;
			const DecodedImage decoded = decode(encoded, [&](double fraction) {
				fractions.push_back(fraction);
				progressLog += QStringLiteral(" %1 ms %2,").arg(timer.elapsed()).arg(fraction, 0, 'f', 2);
				return true;
			});
			const qint64 decodeMs = timer.elapsed();

			const QString report = QStringLiteral("%1, rendering %2: %3x%4, read %5 ms, decode %6 ms %7\nprogress:%8")
				.arg(file.fileName()).arg(static_cast<int>(rawRendering)).arg(decoded.image.width()).arg(decoded.image.height())
				.arg(readMs).arg(decodeMs).arg(decoded.error).arg(progressLog);
			WARN(report.toStdString());
			CHECK_FALSE(decoded.image.isNull());
			CHECK(decoded.format == file.suffix().toLower().toLatin1());
			CHECK(std::ranges::is_sorted(fractions));
			CHECK(std::ranges::all_of(fractions, [](double fraction) { return fraction >= 0.0 && fraction <= 1.0; }));
		}

		const DecodedImage abandoned = decode(readEncodedImage(file.filePath(), RawRendering::Full), [](double fraction) { return fraction < 0.5; });
		CHECK(abandoned.image.isNull());
		CHECK_FALSE(abandoned.error.isEmpty());

		// The rendering's predicted size, which the viewer lays its preview out at, is right at least in aspect ratio
		for (const RawRendering rawRendering : { RawRendering::HalfSize, RawRendering::Full })
		{
			QSize predicted;
			const EncodedImage encoded = readEncodedImage(file.filePath(), rawRendering, [&](EncodedImage, QSize uprightRenderSize) {
				predicted = uprightRenderSize;
				return true;
			});
			if (predicted.isEmpty())
				continue; // no embedded JPEG preview

			const DecodedImage decoded = decode(encoded);
			const bool rotated = decoded.orientation.testFlag(QImageIOHandler::TransformationRotate90);
			const QSize rendered = rotated ? decoded.image.size().transposed() : decoded.image.size();
			WARN(QStringLiteral("%1, rendering %2: predicted %3x%4, rendered %5x%6").arg(file.fileName()).arg(static_cast<int>(rawRendering))
				.arg(predicted.width()).arg(predicted.height()).arg(rendered.width()).arg(rendered.height()).toStdString());
			CHECK(std::abs((double)predicted.width() / predicted.height() - (double)rendered.width() / rendered.height()) <= 0.02 * rendered.width() / rendered.height());
		}
	}
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
