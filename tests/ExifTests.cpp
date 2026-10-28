#include "ImageDecoding/Exif.h"
#include "JpegTestData.h"

#include "3rdparty/catch2/catch.hpp"

#include <QByteArray>

#include <cstdint>
#include <vector>

using ImageDecoding::readJpegExif;
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

[[nodiscard]] bool declaresAdobeRgb(const QByteArray& tiff)
{
	return readJpegExif(jpeg(exifSegment(tiff))).adobeRgb;
}

} // namespace

TEST_CASE("readJpegExif, the orientation: every orientation value, in both byte orders", "[exif]")
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
			REQUIRE(readJpegExif(jpegWithOrientation(value, order)).orientation == transformations);
		}
	}
}

TEST_CASE("readJpegExif, the orientation: found among other entries and segments", "[exif]")
{
	const QByteArray exif = exifSegment(exifTiff(ByteOrder::Big, { { MakeTag, 3 }, { OrientationTag, 6 }, { 0x0131, 8 } }));
	const QByteArray app0 = segment(0xE0, QByteArray(14, '\0'));
	const QByteArray xmp = segment(0xE1, "http://ns.adobe.com/xap/1.0/");

	REQUIRE(readJpegExif(jpeg(app0 + xmp + exif)).orientation == QImageIOHandler::TransformationRotate90);
	REQUIRE(readJpegExif(jpeg(QByteArray{ "\xFF\xFF" } + exif)).orientation == QImageIOHandler::TransformationRotate90); // fill bytes before the marker
}

TEST_CASE("readJpegExif, the orientation: none without an orientation before the scan", "[exif]")
{
	const QByteArray exif = exifSegment(exifTiff(ByteOrder::Little, { { OrientationTag, 6 } }));

	REQUIRE(readJpegExif({}).orientation == QImageIOHandler::TransformationNone);
	REQUIRE(readJpegExif(Soi).orientation == QImageIOHandler::TransformationNone);
	REQUIRE(readJpegExif(jpeg({})).orientation == QImageIOHandler::TransformationNone);
	REQUIRE(readJpegExif(jpeg(exifSegment(exifTiff(ByteOrder::Little, { { MakeTag, 6 } })))).orientation == QImageIOHandler::TransformationNone);
	REQUIRE(readJpegExif(jpeg(segment(0xE1, "http://ns.adobe.com/xap/1.0/"))).orientation == QImageIOHandler::TransformationNone);
	REQUIRE(readJpegExif(Soi + Scan + exif).orientation == QImageIOHandler::TransformationNone);
}

TEST_CASE("readJpegExif, the orientation: malformed input yields none", "[exif]")
{
	const QByteArray tiff = exifTiff(ByteOrder::Little, { { OrientationTag, 6 } });
	const QByteArray exif = exifSegment(tiff);
	REQUIRE(readJpegExif(jpeg(exif)).orientation == QImageIOHandler::TransformationRotate90);

	SECTION("the file cut anywhere")
	{
		const QByteArray whole = jpeg(exif);
		const qsizetype exifEnd = Soi.size() + exif.size();
		for (qsizetype cut = 0; cut < whole.size(); ++cut)
		{
			CAPTURE(cut);
			const Transformations found = readJpegExif(whole.first(cut)).orientation;
			REQUIRE(found == (cut < exifEnd ? QImageIOHandler::TransformationNone : QImageIOHandler::TransformationRotate90));
		}
	}

	SECTION("a segment length below its own two bytes")
	{
		QByteArray damaged = jpeg(exif);
		damaged[Soi.size() + 2] = 0;
		damaged[Soi.size() + 3] = 1;
		REQUIRE(readJpegExif(damaged).orientation == QImageIOHandler::TransformationNone);
	}

	SECTION("a damaged TIFF header")
	{
		const auto withTiffByte = [&](qsizetype offset, char value) {
			QByteArray damaged = tiff;
			damaged[offset] = value;
			return jpeg(exifSegment(damaged));
		};
		REQUIRE(readJpegExif(withTiffByte(0, 'X')).orientation == QImageIOHandler::TransformationNone);  // byte order mark
		REQUIRE(readJpegExif(withTiffByte(2, 43)).orientation == QImageIOHandler::TransformationNone);   // 42
		REQUIRE(readJpegExif(withTiffByte(4, 4)).orientation == QImageIOHandler::TransformationNone);    // IFD0 inside the header
		REQUIRE(readJpegExif(withTiffByte(4, 100)).orientation == QImageIOHandler::TransformationNone);  // IFD0 past the end
		REQUIRE(readJpegExif(jpeg(exifSegment(tiff.first(7)))).orientation == QImageIOHandler::TransformationNone);
	}

	SECTION("an entry count past the end of the data")
	{
		QByteArray damaged = exifTiff(ByteOrder::Little, { { MakeTag, 6 } });
		damaged[8] = 100;
		REQUIRE(readJpegExif(jpeg(exifSegment(damaged))).orientation == QImageIOHandler::TransformationNone);
	}
}

TEST_CASE("readJpegExif, Adobe RGB: ColorSpace 2, or uncalibrated with the interoperability index R03", "[exif]")
{
	for (const ByteOrder order : { ByteOrder::Little, ByteOrder::Big })
	{
		const bool littleEndian = order == ByteOrder::Little;
		CAPTURE(littleEndian);
		REQUIRE(declaresAdobeRgb(exifTiffWithColorSpace(order, 0xFFFF, "R03")));
		REQUIRE(declaresAdobeRgb(exifTiffWithColorSpace(order, 2)));

		REQUIRE_FALSE(declaresAdobeRgb(exifTiffWithColorSpace(order, 1)));          // sRGB
		REQUIRE_FALSE(declaresAdobeRgb(exifTiffWithColorSpace(order, 1, "R03")));
		REQUIRE_FALSE(declaresAdobeRgb(exifTiffWithColorSpace(order, 0xFFFF)));
		REQUIRE_FALSE(declaresAdobeRgb(exifTiffWithColorSpace(order, 0xFFFF, "R98"))); // DCF's sRGB
	}

	REQUIRE_FALSE(readJpegExif(jpegWithOrientation(6)).adobeRgb);
}

TEST_CASE("readJpegExif, Adobe RGB: malformed input yields none", "[exif]")
{
	const QByteArray tiff = exifTiffWithColorSpace(ByteOrder::Little, 0xFFFF, "R03");
	REQUIRE(declaresAdobeRgb(tiff));

	// Every cut through the index or before it; the interoperability IFD's next-IFD offset ends the data and is never read
	for (qsizetype cut = 0; cut < tiff.size() - 4; ++cut)
	{
		CAPTURE(cut);
		REQUIRE_FALSE(declaresAdobeRgb(tiff.first(cut)));
	}

	SECTION("a sub-IFD offset past the end")
	{
		QByteArray damaged = tiff;
		damaged[8 + 2 + 8] = 100; // IFD0's Exif IFD pointer
		REQUIRE_FALSE(declaresAdobeRgb(damaged));
	}
}
