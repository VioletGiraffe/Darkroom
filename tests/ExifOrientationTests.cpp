#include "ImageDecoding/ExifOrientation.h"
#include "JpegTestData.h"

#include "3rdparty/catch2/catch.hpp"

#include <QByteArray>

#include <cstdint>

using ImageDecoding::exifOrientation;
using Transformations = QImageIOHandler::Transformations;

namespace {

const QByteArray Soi{ "\xFF\xD8" };
const QByteArray Scan = segment(0xDA, QByteArray(10, '\0')) + QByteArray{ "\x12\x34\xFF\xD9" }; // SOS, entropy-coded data, EOI

[[nodiscard]] QByteArray jpeg(const QByteArray& segments)
{
	return Soi + segments + Scan;
}

[[nodiscard]] QByteArray jpegWithOrientation(uint16_t orientation, ByteOrder order = ByteOrder::Little)
{
	return jpeg(exifSegment(exifTiff(order, { { OrientationTag, orientation } })));
}

} // namespace

TEST_CASE("exifOrientation: every orientation value, in both byte orders", "[exif]")
{
	struct Expected
	{
		uint16_t value;
		Transformations transformations;
	};
	const Expected expected[]{
		{ 1, QImageIOHandler::TransformationNone },
		{ 2, QImageIOHandler::TransformationMirror },
		{ 3, QImageIOHandler::TransformationRotate180 },
		{ 4, QImageIOHandler::TransformationFlip },
		{ 5, QImageIOHandler::TransformationFlipAndRotate90 },
		{ 6, QImageIOHandler::TransformationRotate90 },
		{ 7, QImageIOHandler::TransformationMirrorAndRotate90 },
		{ 8, QImageIOHandler::TransformationRotate270 },
		// Outside the range EXIF defines
		{ 0, QImageIOHandler::TransformationNone },
		{ 9, QImageIOHandler::TransformationNone },
	};

	for (const ByteOrder order : { ByteOrder::Little, ByteOrder::Big })
	{
		for (const auto& [value, transformations] : expected)
		{
			const bool littleEndian = order == ByteOrder::Little;
			CAPTURE(littleEndian, value);
			REQUIRE(exifOrientation(jpegWithOrientation(value, order)) == transformations);
		}
	}
}

TEST_CASE("exifOrientation: found among other entries and segments", "[exif]")
{
	const QByteArray exif = exifSegment(exifTiff(ByteOrder::Big, { { MakeTag, 3 }, { OrientationTag, 6 }, { 0x0131, 8 } }));
	const QByteArray app0 = segment(0xE0, QByteArray(14, '\0'));
	const QByteArray xmp = segment(0xE1, "http://ns.adobe.com/xap/1.0/");

	REQUIRE(exifOrientation(jpeg(app0 + xmp + exif)) == QImageIOHandler::TransformationRotate90);
	REQUIRE(exifOrientation(jpeg(QByteArray{ "\xFF\xFF" } + exif)) == QImageIOHandler::TransformationRotate90); // fill bytes before the marker
}

TEST_CASE("exifOrientation: none without an orientation before the scan", "[exif]")
{
	const QByteArray exif = exifSegment(exifTiff(ByteOrder::Little, { { OrientationTag, 6 } }));

	REQUIRE(exifOrientation({}) == QImageIOHandler::TransformationNone);
	REQUIRE(exifOrientation(Soi) == QImageIOHandler::TransformationNone);
	REQUIRE(exifOrientation(jpeg({})) == QImageIOHandler::TransformationNone);
	REQUIRE(exifOrientation(jpeg(exifSegment(exifTiff(ByteOrder::Little, { { MakeTag, 6 } })))) == QImageIOHandler::TransformationNone);
	REQUIRE(exifOrientation(jpeg(segment(0xE1, "http://ns.adobe.com/xap/1.0/"))) == QImageIOHandler::TransformationNone);
	REQUIRE(exifOrientation(Soi + Scan + exif) == QImageIOHandler::TransformationNone);
}

TEST_CASE("exifOrientation: malformed input yields none", "[exif]")
{
	const QByteArray tiff = exifTiff(ByteOrder::Little, { { OrientationTag, 6 } });
	const QByteArray exif = exifSegment(tiff);
	REQUIRE(exifOrientation(jpeg(exif)) == QImageIOHandler::TransformationRotate90);

	SECTION("the file cut anywhere")
	{
		const QByteArray whole = jpeg(exif);
		const qsizetype exifEnd = Soi.size() + exif.size();
		for (qsizetype cut = 0; cut < whole.size(); ++cut)
		{
			CAPTURE(cut);
			const Transformations found = exifOrientation(whole.first(cut));
			REQUIRE(found == (cut < exifEnd ? QImageIOHandler::TransformationNone : QImageIOHandler::TransformationRotate90));
		}
	}

	SECTION("a segment length below its own two bytes")
	{
		QByteArray damaged = jpeg(exif);
		damaged[Soi.size() + 2] = 0;
		damaged[Soi.size() + 3] = 1;
		REQUIRE(exifOrientation(damaged) == QImageIOHandler::TransformationNone);
	}

	SECTION("a damaged TIFF header")
	{
		const auto withTiffByte = [&](qsizetype offset, char value) {
			QByteArray damaged = tiff;
			damaged[offset] = value;
			return jpeg(exifSegment(damaged));
		};
		REQUIRE(exifOrientation(withTiffByte(0, 'X')) == QImageIOHandler::TransformationNone);  // byte order mark
		REQUIRE(exifOrientation(withTiffByte(2, 43)) == QImageIOHandler::TransformationNone);   // 42
		REQUIRE(exifOrientation(withTiffByte(4, 4)) == QImageIOHandler::TransformationNone);    // IFD0 inside the header
		REQUIRE(exifOrientation(withTiffByte(4, 100)) == QImageIOHandler::TransformationNone);  // IFD0 past the end
		REQUIRE(exifOrientation(jpeg(exifSegment(tiff.first(7)))) == QImageIOHandler::TransformationNone);
	}

	SECTION("an entry count past the end of the data")
	{
		QByteArray damaged = exifTiff(ByteOrder::Little, { { MakeTag, 6 } });
		damaged[8] = 100;
		REQUIRE(exifOrientation(jpeg(exifSegment(damaged))) == QImageIOHandler::TransformationNone);
	}
}
