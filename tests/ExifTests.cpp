#include "ImageDecoding/ImageDecoding.h"
#include "ImageDecoding/LibRawDecoder.h"
#include "ImageMetadata/CaptureMetadata.h"
#include "ImageMetadata/EmbeddedMetadata.h"
#include "ImageMetadata/Exif.h"
#include "ImageMetadata/ExifTagNames.h"
#include "ImageMetadata/ExifValues.h"
#include "ImageMetadata/ImageInfo.h"
#include "JpegTestData.h"
#include "buffered_reader.hpp" // thin_io
#include "qt_paths.hpp"        // thin_io

#include "3rdparty/catch2/catch.hpp"

#include <QBuffer>
#include <QByteArray>
#include <QColorSpace>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QSize>
#include <QString>
#include <QTemporaryDir>
#include <QtEndian>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <optional>
#include <utility>
#include <vector>

using namespace ImageMetadata;
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

constexpr uint16_t ByteType = 1, AsciiType = 2, ShortType = 3, LongType = 4, RationalType = 5, UndefinedType = 7, SRationalType = 10,
	FloatType = 11, DoubleType = 12;
constexpr uint16_t MakerNoteTag = 0x927C, PixelXDimensionTag = 0xA002, PixelYDimensionTag = 0xA003;

struct TestEntry
{
	uint16_t tag;
	uint16_t type;
	uint32_t count;
	QByteArray value; // in the TIFF's byte order
};

// IFD0 at offset 8, then the Exif, GPS and interoperability IFDs and IFD1, each written when given entries, with the pointers to them.
// Every value that does not fit its entry follows all the IFDs, unaligned: a layout unlike exifApp1FromTags()'s.
[[nodiscard]] QByteArray tiffWithIfds(ByteOrder order, std::vector<TestEntry> ifd0, std::vector<TestEntry> exif = {}, std::vector<TestEntry> gps = {},
	std::vector<TestEntry> interoperability = {}, std::vector<TestEntry> ifd1 = {})
{
	if (!interoperability.empty())
		exif.push_back({ 0xA005, LongType, 1, {} });
	if (!exif.empty())
		ifd0.push_back({ 0x8769, LongType, 1, {} });
	if (!gps.empty())
		ifd0.push_back({ 0x8825, LongType, 1, {} });

	const std::vector<std::vector<TestEntry>*> ifds{ &ifd0, &exif, &gps, &interoperability, &ifd1 };
	std::vector<uint32_t> offsets;
	uint32_t valuesOffset = 8;
	for (const std::vector<TestEntry>* ifd : ifds)
	{
		offsets.push_back(ifd->empty() ? 0 : valuesOffset);
		if (!ifd->empty())
			valuesOffset += static_cast<uint32_t>(2 + 12 * ifd->size() + 4);
	}

	const auto setPointer = [&](std::vector<TestEntry>& ifd, uint16_t tag, uint32_t target) {
		for (TestEntry& entry : ifd)
		{
			if (entry.tag == tag)
				entry.value = bytesOf(target, order);
		}
	};
	setPointer(exif, 0xA005, offsets[3]);
	setPointer(ifd0, 0x8769, offsets[1]);
	setPointer(ifd0, 0x8825, offsets[2]);

	QByteArray tiff = (order == ByteOrder::Little ? "II" : "MM") + bytesOf<uint16_t>(42, order) + bytesOf<uint32_t>(8, order);
	QByteArray values;
	for (size_t i = 0; i < ifds.size(); ++i)
	{
		const std::vector<TestEntry>& ifd = *ifds[i];
		if (ifd.empty())
			continue;

		tiff += bytesOf(static_cast<uint16_t>(ifd.size()), order);
		for (const TestEntry& entry : ifd)
		{
			tiff += bytesOf(entry.tag, order) + bytesOf(entry.type, order) + bytesOf(entry.count, order);
			if (entry.value.size() <= 4)
			{
				tiff += entry.value.leftJustified(4, '\0');
			}
			else
			{
				tiff += bytesOf(static_cast<uint32_t>(valuesOffset + values.size()), order);
				values += entry.value;
			}
		}
		tiff += bytesOf(i == 0 ? offsets[4] : uint32_t{ 0 }, order); // IFD0's next IFD is IFD1
	}
	return tiff + values;
}

[[nodiscard]] QByteArray exifPayload(const QByteArray& tiff)
{
	return QByteArray{ "Exif\0\0", 6 } + tiff;
}

// With the terminating NUL
[[nodiscard]] QByteArray ascii(const char* text)
{
	return QByteArray{ text, static_cast<qsizetype>(std::strlen(text) + 1) };
}

// Entries in every directory, with values in and out of their entries; the MakerNote's length is odd
struct SampleIfds
{
	std::vector<TestEntry> ifd0, exif, gps, interoperability, ifd1;
};

[[nodiscard]] SampleIfds sampleIfds(ByteOrder order)
{
	return {
		.ifd0 = { { MakeTag, AsciiType, 6, ascii("Canon") }, { OrientationTag, ShortType, 1, bytesOf<uint16_t>(6, order) } },
		.exif = {
			{ 0x829A, RationalType, 1, bytesOf<uint32_t>(1, order) + bytesOf<uint32_t>(250, order) }, // ExposureTime
			{ 0xA001, ShortType, 1, bytesOf<uint16_t>(1, order) },                                    // ColorSpace
			{ MakerNoteTag, UndefinedType, 9, QByteArray{ "MakerNote", 9 } },
		},
		.gps = { { 0x0000, ByteType, 4, QByteArray{ "\x02\x03\x00\x00", 4 } } }, // GPSVersionID
		.interoperability = { { 0x0001, AsciiType, 4, ascii("R98") } },
		.ifd1 = { { 0x0201, LongType, 1, bytesOf<uint32_t>(1234, order) } }, // JPEGInterchangeFormat
	};
}

[[nodiscard]] QByteArray sampleTiff(ByteOrder order)
{
	const SampleIfds ifds = sampleIfds(order);
	return tiffWithIfds(order, ifds.ifd0, ifds.exif, ifds.gps, ifds.interoperability, ifds.ifd1);
}

// sampleTiff()'s entries in the order exifTagsFromApp1() reads them: each sub-IFD where IFD0 or the Exif IFD points to it, IFD1 last
[[nodiscard]] ExifTags sampleTags(ByteOrder order)
{
	const SampleIfds ifds = sampleIfds(order);
	ExifTags tags{ .littleEndian = order == ByteOrder::Little };
	const auto add = [&](ExifDirectory directory, const std::vector<TestEntry>& entries) {
		for (const TestEntry& entry : entries)
			tags.entries.push_back({ directory, entry.tag, entry.type, entry.count, entry.value });
	};
	add(ExifDirectory::Ifd0, ifds.ifd0);
	add(ExifDirectory::Exif, ifds.exif);
	add(ExifDirectory::Interoperability, ifds.interoperability);
	add(ExifDirectory::Gps, ifds.gps);
	add(ExifDirectory::Ifd1, ifds.ifd1);
	return tags;
}

// By directory, then tag: exifApp1FromTags() sorts each IFD's entries
[[nodiscard]] std::vector<ExifEntry> sortedEntries(std::vector<ExifEntry> entries)
{
	std::ranges::sort(entries, {}, [](const ExifEntry& entry) { return std::pair{ entry.directory, entry.tag }; });
	return entries;
}

[[nodiscard]] const ExifEntry* findEntry(const ExifTags& tags, ExifDirectory directory, uint16_t tag)
{
	const auto entry = std::ranges::find_if(tags.entries, [&](const ExifEntry& e) { return e.directory == directory && e.tag == tag; });
	return entry != tags.entries.end() ? &*entry : nullptr;
}

void removeDirectory(ExifTags& tags, ExifDirectory directory)
{
	std::erase_if(tags.entries, [&](const ExifEntry& entry) { return entry.directory == directory; });
}

// The EXIF of a JPEG source: Orientation, PixelXDimension as a SHORT, PixelYDimension as a LONG, a MakerNote, and IFD1
[[nodiscard]] QByteArray jpegSourceTiff(ByteOrder order, uint16_t orientation)
{
	return tiffWithIfds(order,
		{ { MakeTag, AsciiType, 6, ascii("Canon") }, { OrientationTag, ShortType, 1, bytesOf(orientation, order) } },
		{
			{ MakerNoteTag, UndefinedType, 9, QByteArray{ "MakerNote", 9 } },
			{ PixelXDimensionTag, ShortType, 1, bytesOf<uint16_t>(6000, order) },
			{ PixelYDimensionTag, LongType, 1, bytesOf<uint32_t>(4000, order) },
		},
		{}, {},
		{ { 0x0201, LongType, 1, bytesOf<uint32_t>(1234, order) } });
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

TEST_CASE("exifTagsFromApp1: the entries of every directory, in both byte orders", "[exif]")
{
	for (const ByteOrder order : { ByteOrder::Little, ByteOrder::Big })
	{
		const bool littleEndian = order == ByteOrder::Little;
		CAPTURE(littleEndian);
		REQUIRE(exifTagsFromApp1(exifPayload(sampleTiff(order))) == sampleTags(order));
	}
}

TEST_CASE("exifTagsFromApp1: no entries unless the payload holds EXIF", "[exif]")
{
	REQUIRE(exifTagsFromApp1({}).entries.empty());
	REQUIRE(exifTagsFromApp1("http://ns.adobe.com/xap/1.0/").entries.empty());
	REQUIRE(exifTagsFromApp1(sampleTiff(ByteOrder::Little)).entries.empty()); // no Exif header
	REQUIRE(exifTagsFromApp1(exifPayload(QByteArray{ "XX\x2A\0\x08\0\0\0", 8 })).entries.empty());
}

TEST_CASE("exifTagsFromApp1: malformed input keeps the entries it can read", "[exif]")
{
	const QByteArray payload = exifPayload(sampleTiff(ByteOrder::Little));
	const ExifTags whole = exifTagsFromApp1(payload);

	SECTION("the payload cut anywhere")
	{
		for (qsizetype cut = 0; cut < payload.size(); ++cut)
		{
			CAPTURE(cut);
			for (const ExifEntry& entry : exifTagsFromApp1(payload.first(cut)).entries)
				REQUIRE(std::ranges::contains(whole.entries, entry));
		}
	}

	SECTION("a value offset past the end")
	{
		QByteArray damaged = payload;
		damaged.replace(6 + 8 + 2 + 8, 4, QByteArray(4, '\xFF')); // Make's value offset
		ExifTags expected = whole;
		std::erase_if(expected.entries, [](const ExifEntry& entry) { return entry.tag == MakeTag; });
		REQUIRE(exifTagsFromApp1(damaged) == expected);
	}
}

TEST_CASE("exifApp1FromTags: reads back as the same entries without IFD1, each IFD sorted by tag", "[exif]")
{
	for (const ByteOrder order : { ByteOrder::Little, ByteOrder::Big })
	{
		const bool littleEndian = order == ByteOrder::Little;
		CAPTURE(littleEndian);
		const QByteArray payload = exifApp1FromTags(sampleTags(order));
		REQUIRE(payload.startsWith(QByteArray{ "Exif\0\0", 6 }));

		const ExifTags read = exifTagsFromApp1(payload);
		ExifTags expected = sampleTags(order);
		removeDirectory(expected, ExifDirectory::Ifd1);
		REQUIRE(read.littleEndian == littleEndian);
		REQUIRE(sortedEntries(read.entries) == sortedEntries(expected.entries));

		for (const ExifDirectory directory : { ExifDirectory::Ifd0, ExifDirectory::Exif, ExifDirectory::Gps, ExifDirectory::Interoperability })
		{
			std::vector<uint16_t> tags;
			for (const ExifEntry& entry : read.entries)
			{
				if (entry.directory == directory)
					tags.push_back(entry.tag);
			}
			REQUIRE(std::ranges::is_sorted(tags));
		}
	}
}

TEST_CASE("exifApp1FromTags: every IFD and every value outside its entry starts on a word boundary", "[exif]")
{
	const QByteArray app1 = exifApp1FromTags(sampleTags(ByteOrder::Little));
	// A copy: GCC's -Warray-bounds follows QByteArray::data() into its 1-byte empty-array fallback and flags these reads
	const std::vector<char> tiff(app1.cbegin() + 6, app1.cend());
	const auto read16 = [&](qsizetype offset) { return qFromLittleEndian<uint16_t>(tiff.data() + offset); };
	const auto read32 = [&](qsizetype offset) { return qFromLittleEndian<uint32_t>(tiff.data() + offset); };
	const auto typeSize = [](uint16_t type) { return type == ShortType ? 2 : type == LongType ? 4 : type == RationalType ? 8 : 1; };

	std::vector<uint32_t> ifds{ read32(4) };
	for (size_t i = 0; i < ifds.size(); ++i)
	{
		const uint32_t ifd = ifds[i];
		CAPTURE(ifd);
		REQUIRE(ifd % 2 == 0);
		for (int e = 0; e < read16(ifd); ++e)
		{
			const qsizetype entry = ifd + 2 + 12 * e;
			const uint16_t tag = read16(entry);
			if ((i == 0 && (tag == 0x8769 || tag == 0x8825)) || tag == 0xA005)
				ifds.push_back(read32(entry + 8));
			else if (read32(entry + 4) * typeSize(read16(entry + 2)) > 4)
				REQUIRE(read32(entry + 8) % 2 == 0);
		}
	}
	REQUIRE(ifds.size() == 4);
}

TEST_CASE("exifApp1FromTags: empty without entries to write, or when they do not fit a segment", "[exif]")
{
	REQUIRE(exifApp1FromTags({}).isEmpty());

	ExifTags onlyIfd1 = sampleTags(ByteOrder::Little);
	std::erase_if(onlyIfd1.entries, [](const ExifEntry& entry) { return entry.directory != ExifDirectory::Ifd1; });
	REQUIRE(exifApp1FromTags(onlyIfd1).isEmpty());

	// The Exif header, the TIFF header and an IFD0 of one entry take 32 bytes of the 65533 a segment holds; an odd value is padded
	const auto payloadWithValueOf = [](qsizetype size) {
		return exifApp1FromTags({ .entries = { { ExifDirectory::Ifd0, 0x9C9C, ByteType, static_cast<uint32_t>(size), QByteArray(size, 'x') } } });
	};
	REQUIRE(payloadWithValueOf(65500).size() == 65532);
	REQUIRE(payloadWithValueOf(65501).isEmpty());
}

TEST_CASE("prepareExifTagsForOutput: keeps what describes the output image", "[exif]")
{
	for (const ByteOrder order : { ByteOrder::Little, ByteOrder::Big })
	{
		const bool littleEndian = order == ByteOrder::Little;
		CAPTURE(littleEndian);
		ExifTags tags = sampleTags(order);
		tags.entries.push_back({ ExifDirectory::Ifd0, 0x0111, LongType, 1, bytesOf<uint32_t>(5000, order) });          // StripOffsets
		tags.entries.push_back({ ExifDirectory::Ifd0, 0xC612, ByteType, 4, QByteArray{ "\x01\x04\x00\x00", 4 } });      // DNGVersion
		tags.entries.push_back({ ExifDirectory::Exif, PixelXDimensionTag, ShortType, 1, bytesOf<uint16_t>(6000, order) }); // without PixelYDimension
		prepareExifTagsForOutput(tags, QSize{ 300, 200 });

		ExifTags expected = sampleTags(order);
		removeDirectory(expected, ExifDirectory::Ifd1);
		std::erase_if(expected.entries, [](const ExifEntry& entry) { return entry.tag == MakerNoteTag; });
		for (ExifEntry& entry : expected.entries)
		{
			if (entry.tag == OrientationTag)
				entry.value = bytesOf<uint16_t>(1, order);
		}
		expected.entries.push_back({ ExifDirectory::Exif, PixelXDimensionTag, LongType, 1, bytesOf<uint32_t>(300, order) });
		expected.entries.push_back({ ExifDirectory::Exif, PixelYDimensionTag, LongType, 1, bytesOf<uint32_t>(200, order) });
		REQUIRE(sortedEntries(tags.entries) == sortedEntries(expected.entries));
	}
}

TEST_CASE("prepareExifTagsForOutput: adds no orientation, and no pixel dimensions without other Exif entries", "[exif]")
{
	const ExifTags make{ .entries = { { ExifDirectory::Ifd0, MakeTag, AsciiType, 6, ascii("Canon") } } };
	ExifTags tags = make;
	prepareExifTagsForOutput(tags, QSize{ 300, 200 });
	REQUIRE(tags == make);
}

TEST_CASE("exifApp1ForOutput, a JPEG source: its own payload, edited in place", "[exif]")
{
	for (const ByteOrder order : { ByteOrder::Little, ByteOrder::Big })
	{
		for (const uint16_t orientation : { uint16_t{ 1 }, uint16_t{ 6 } })
		{
			const bool littleEndian = order == ByteOrder::Little;
			CAPTURE(littleEndian, orientation);
			const QByteArray sourceTiff = jpegSourceTiff(order, orientation);
			const QByteArray output = exifApp1ForOutput(jpeg(exifSegment(sourceTiff)), QSize{ 300, 200 });
			REQUIRE(output.size() == exifPayload(sourceTiff).size());

			ExifTags expected = exifTagsFromApp1(exifPayload(sourceTiff));
			for (ExifEntry& entry : expected.entries)
			{
				if (entry.tag == OrientationTag)
					entry.value = bytesOf<uint16_t>(1, order);
				else if (entry.tag == PixelXDimensionTag)
					entry.value = bytesOf<uint16_t>(300, order);
				else if (entry.tag == PixelYDimensionTag)
					entry.value = bytesOf<uint32_t>(200, order);
			}
			// The thumbnail keeps the source's orientation
			if (orientation != 1)
				removeDirectory(expected, ExifDirectory::Ifd1);
			REQUIRE(exifTagsFromApp1(output) == expected);
		}
	}
}

TEST_CASE("exifApp1ForOutput: empty for a JPEG without EXIF, or a file LibRaw cannot open", "[exif]")
{
	REQUIRE(exifApp1ForOutput(jpeg({}), QSize{ 300, 200 }).isEmpty());
	REQUIRE(exifApp1ForOutput(jpeg(segment(0xE1, "http://ns.adobe.com/xap/1.0/")), QSize{ 300, 200 }).isEmpty());
	REQUIRE(exifApp1ForOutput(QByteArray{ "not an image" }, QSize{ 300, 200 }).isEmpty());
}

TEST_CASE("jpegWithExifApp1: the segment follows SOI, and APP0 when present", "[exif]")
{
	const QByteArray payload = exifPayload(sampleTiff(ByteOrder::Little));
	const QByteArray app0 = segment(0xE0, QByteArray{ "JFIF\0\x01\x01\0\0\x01\0\x01\0\0", 14 });

	REQUIRE(jpegWithExifApp1(jpeg({}), payload) == jpeg(segment(0xE1, payload)));
	REQUIRE(jpegWithExifApp1(jpeg(app0), payload) == jpeg(app0 + segment(0xE1, payload)));
	REQUIRE(jpegWithExifApp1(jpeg(app0), {}) == jpeg(app0));

	const QByteArray written = jpegWithExifApp1(jpeg(app0), payload);
	const std::optional<QByteArrayView> found = findExifApp1(written);
	REQUIRE(found);
	REQUIRE(found->toByteArray() == payload);
}

// Hidden: reads the EXIF of every camera raw at the top level of the folder named by DARKROOM_RAW_SAMPLES, reporting what each directory holds
TEST_CASE("exifTagsFromRaw: camera raw samples", "[.][rawexif]")
{
	const QString folder = qEnvironmentVariable("DARKROOM_RAW_SAMPLES");
	REQUIRE_FALSE(folder.isEmpty());

	for (const QFileInfo& file : QDir{ folder }.entryInfoList(QDir::Files, QDir::Name))
	{
		if (!ImageDecoding::rawFileSuffixes().contains(file.suffix().toLower()))
			continue;

		QFile input{ file.filePath() };
		REQUIRE(input.open(QIODevice::ReadOnly));
		const QByteArray bytes = input.readAll();
		const ExifTags tags = exifTagsFromRaw(bytes);

		const auto entryCount = [&](ExifDirectory directory) { return std::ranges::count(tags.entries, directory, &ExifEntry::directory); };
		const auto text = [&](ExifDirectory directory, uint16_t tag) {
			const ExifEntry* entry = findEntry(tags, directory, tag);
			return entry ? QString::fromLatin1(entry->value.constData()) : QStringLiteral("none");
		};
		WARN(QStringLiteral("%1: IFD0 %2, Exif %3, GPS %4, interoperability %5 entries; Make %6, DateTimeOriginal %7")
			.arg(file.fileName()).arg(entryCount(ExifDirectory::Ifd0)).arg(entryCount(ExifDirectory::Exif)).arg(entryCount(ExifDirectory::Gps))
			.arg(entryCount(ExifDirectory::Interoperability)).arg(text(ExifDirectory::Ifd0, MakeTag), text(ExifDirectory::Exif, 0x9003)).toStdString());
		CHECK(findEntry(tags, ExifDirectory::Ifd0, MakeTag));
		CHECK(findEntry(tags, ExifDirectory::Exif, 0x9003)); // DateTimeOriginal
		CHECK_FALSE(findEntry(tags, ExifDirectory::Exif, MakerNoteTag));

		thin_io::buffered_reader reader{ ImageDecoding::LibRawReadWindowSize };
		REQUIRE(reader.open(thin_io::native_path(file.filePath()).data()));
		const std::optional<ImageDecoding::RawMetadata> metadata = ImageDecoding::readRawMetadata(reader);
		REQUIRE(metadata);
		CHECK(metadata->exif == tags);
		CHECK_FALSE(metadata->uprightSize.isEmpty());
		const std::optional<LensSpecification>& lens = metadata->lensSpecification;
		const QString lensSpecification = lens ? QStringLiteral("%1-%2 mm f/%3-%4").arg(lens->minFocalLength).arg(lens->maxFocalLength)
			.arg(lens->fNumberAtMinFocal.value_or(0)).arg(lens->fNumberAtMaxFocal.value_or(0)) : QStringLiteral("none");
		WARN(QStringLiteral("%1: lens \"%2\", lens specification %3, lens serial \"%4\", body serial \"%5\", ICC profile %6 bytes").arg(file.fileName(),
			metadata->lens, lensSpecification, metadata->lensSerial, metadata->bodySerial).arg(metadata->iccProfile.size()).toStdString());

		const QSize outputSize{ 1200, 800 };
		ExifTags expected = tags;
		prepareExifTagsForOutput(expected, outputSize);
		const QByteArray payload = exifApp1ForOutput(bytes, outputSize);
		CHECK_FALSE(payload.isEmpty());
		CHECK(sortedEntries(exifTagsFromApp1(payload).entries) == sortedEntries(expected.entries));
	}
}

namespace {

[[nodiscard]] QByteArray rational(uint32_t numerator, uint32_t denominator, ByteOrder order)
{
	return bytesOf(numerator, order) + bytesOf(denominator, order);
}

[[nodiscard]] QByteArray pngChunk(const char* type, const QByteArray& data)
{
	return bytesOf(static_cast<uint32_t>(data.size()), ByteOrder::Big) + type + data + QByteArray(4, '\0'); // the CRC is not checked
}

[[nodiscard]] QByteArray webpChunk(const char* type, const QByteArray& data)
{
	QByteArray chunk = type + bytesOf(static_cast<uint32_t>(data.size()), ByteOrder::Little) + data;
	return data.size() % 2 ? chunk + '\0' : chunk;
}

[[nodiscard]] QByteArray webp(const QByteArray& chunks)
{
	return "RIFF" + bytesOf(static_cast<uint32_t>(4 + chunks.size()), ByteOrder::Little) + "WEBP" + chunks;
}

// An APP2 segment holding one part of an ICC profile
[[nodiscard]] QByteArray iccSegment(uint8_t sequence, uint8_t count, const QByteArray& part)
{
	return segment(0xE2, QByteArray{ "ICC_PROFILE\0", 12 } + static_cast<char>(sequence) + static_cast<char>(count) + part);
}

[[nodiscard]] ExifEntry exifEntry(ExifDirectory directory, uint16_t tag, uint16_t type, uint32_t count, QByteArray value)
{
	return { directory, tag, type, count, std::move(value) };
}

} // namespace

TEST_CASE("readEmbeddedMetadata, JPEG: the EXIF, and the ICC profile joined from its parts in sequence order", "[exif]")
{
	const QByteArray tiff = sampleTiff(ByteOrder::Little);
	const EmbeddedMetadata metadata = readEmbeddedMetadata(jpeg(exifSegment(tiff) + iccSegment(2, 2, "-second") + iccSegment(1, 2, "first")));
	CHECK(metadata.exif == exifTagsFromTiff(tiff));
	CHECK(metadata.iccProfile == "first-second");

	SECTION("a profile missing a part is left out")
	{
		CHECK(readEmbeddedMetadata(jpeg(iccSegment(1, 2, "first"))).iccProfile.isEmpty());
		CHECK(readEmbeddedMetadata(jpeg(iccSegment(2, 2, "-second") + iccSegment(3, 2, "third"))).iccProfile.isEmpty());
	}
}

TEST_CASE("readEmbeddedMetadata, TIFF: the file's own directories, and InterColorProfile", "[exif]")
{
	for (const ByteOrder order : { ByteOrder::Little, ByteOrder::Big })
	{
		const QByteArray profile{ "profile data" };
		const QByteArray tiff = tiffWithIfds(order, { { MakeTag, AsciiType, 6, ascii("Canon") }, { 0x8773, UndefinedType, 12, profile } });
		const EmbeddedMetadata metadata = readEmbeddedMetadata(tiff);
		CHECK(metadata.exif == exifTagsFromTiff(tiff));
		CHECK(metadata.exif.entries.size() == 2);
		CHECK(metadata.iccProfile == profile);
	}
}

TEST_CASE("readEmbeddedMetadata, PNG: eXIf with or without APP1's header, and iCCP decompressed", "[exif]")
{
	const QByteArray tiff = sampleTiff(ByteOrder::Big);
	const QByteArray profile = QByteArray{ "an ICC profile, " }.repeated(20);
	// qCompress puts the size first, then the zlib stream iCCP holds
	const QByteArray iccp = QByteArray{ "Profile name" } + '\0' + '\0' + qCompress(profile).sliced(4);
	const QByteArray header = QByteArray{ "\x89PNG\r\n\x1A\n", 8 } + pngChunk("IHDR", QByteArray(13, '\0'));

	for (const QByteArray& exifChunk : { tiff, exifPayload(tiff) })
	{
		const EmbeddedMetadata metadata = readEmbeddedMetadata(header + pngChunk("iCCP", iccp) + pngChunk("IDAT", "x") + pngChunk("eXIf", exifChunk)
			+ pngChunk("IEND", {}));
		CHECK(metadata.exif == exifTagsFromTiff(tiff));
		CHECK(metadata.iccProfile == profile);
	}

	SECTION("a chunk running past the end ends the walk")
	{
		const QByteArray truncated = header + pngChunk("eXIf", tiff).chopped(5);
		CHECK(readEmbeddedMetadata(truncated).exif.entries.empty());
	}
}

TEST_CASE("readEmbeddedMetadata, WebP: EXIF with or without APP1's header, and ICCP, after an odd-sized chunk", "[exif]")
{
	const QByteArray tiff = sampleTiff(ByteOrder::Little);
	for (const QByteArray& exifChunk : { tiff, exifPayload(tiff) })
	{
		const EmbeddedMetadata metadata = readEmbeddedMetadata(webp(webpChunk("VP8X", "odd") + webpChunk("ICCP", "profile") + webpChunk("EXIF", exifChunk)));
		CHECK(metadata.exif == exifTagsFromTiff(tiff));
		CHECK(metadata.iccProfile == "profile");
	}
}

TEST_CASE("readEmbeddedMetadata: nothing from other files", "[exif]")
{
	CHECK(readEmbeddedMetadata("GIF89a").exif.entries.empty());
	CHECK(readEmbeddedMetadata({}).iccProfile.isEmpty());
}

TEST_CASE("exifTagName: each directory's table", "[exif]")
{
	CHECK(exifTagName(ExifDirectory::Ifd0, MakeTag) == "Make");
	CHECK(exifTagName(ExifDirectory::Exif, 0x829A) == "ExposureTime");
	CHECK(exifTagName(ExifDirectory::Ifd0, 0xC612) == "DNGVersion");
	CHECK(exifTagName(ExifDirectory::Ifd1, 0x0201) == "JPEGInterchangeFormat");
	CHECK(exifTagName(ExifDirectory::Gps, 0x0001) == "GPSLatitudeRef");
	CHECK(exifTagName(ExifDirectory::Interoperability, 0x0001) == "InteroperabilityIndex");
	CHECK(exifTagName(ExifDirectory::Ifd0, 0x0001).empty());
	CHECK(exifTagName(ExifDirectory::Exif, 0xFFFF).empty());
}

TEST_CASE("exifEntryValue: numbers in the entry's byte order, as many as the value holds", "[exif]")
{
	using Integers = std::vector<int64_t>;
	for (const ByteOrder order : { ByteOrder::Little, ByteOrder::Big })
	{
		const bool littleEndian = order == ByteOrder::Little;
		const auto value = [&](uint16_t type, uint32_t count, const QByteArray& bytes) {
			return exifEntryValue(exifEntry(ExifDirectory::Exif, 0x1234, type, count, bytes), littleEndian);
		};
		CHECK(value(ShortType, 2, bytesOf<uint16_t>(3, order) + bytesOf<uint16_t>(65535, order)) == ExifValue{ Integers{ 3, 65535 } });
		CHECK(value(LongType, 1, bytesOf<uint32_t>(4000000000, order)) == ExifValue{ Integers{ 4000000000 } });
		CHECK(value(9, 1, bytesOf<int32_t>(-5, order)) == ExifValue{ Integers{ -5 } }); // SLONG
		CHECK(value(ByteType, 4, QByteArray{ "\x02\x03\x00\xFF", 4 }) == ExifValue{ Integers{ 2, 3, 0, 255 } });
		CHECK(value(RationalType, 1, rational(1, 250, order)) == ExifValue{ std::vector<ExifRational>{ { 1, 250 } } });
		CHECK(value(SRationalType, 1, bytesOf<int32_t>(-2, order) + bytesOf<int32_t>(3, order)) == ExifValue{ std::vector<ExifRational>{ { -2, 3 } } });
		CHECK(value(FloatType, 1, bytesOf(std::bit_cast<uint32_t>(1.5f), order)) == ExifValue{ std::vector<double>{ 1.5 } });
		CHECK(value(DoubleType, 1, bytesOf(std::bit_cast<uint64_t>(-0.25), order)) == ExifValue{ std::vector<double>{ -0.25 } });
		// A count past the value
		CHECK(value(ShortType, 3, bytesOf<uint16_t>(7, order)) == ExifValue{ Integers{ 7 } });
	}
}

TEST_CASE("exifEntryValue: text", "[exif]")
{
	const auto value = [](ExifDirectory directory, uint16_t tag, uint16_t type, const QByteArray& bytes, bool littleEndian = true) {
		return exifEntryValue(exifEntry(directory, tag, type, static_cast<uint32_t>(bytes.size()), bytes), littleEndian);
	};
	SECTION("ASCII: NUL-separated strings, trimmed, the empty ones left out")
	{
		CHECK(value(ExifDirectory::Ifd0, 0x8298, AsciiType, QByteArray{ "Photographer \0\0 Editor\0", 24 }) == ExifValue{ QString{ "Photographer; Editor" } });
		CHECK(value(ExifDirectory::Ifd0, 0x010E, AsciiType, QByteArray{ "    \0", 5 }) == ExifValue{ QString{} });
	}
	SECTION("ASCII: UTF-8 when valid, else Latin-1")
	{
		CHECK(value(ExifDirectory::Ifd0, 0x013B, AsciiType, ascii("Jos\xC3\xA9")) == ExifValue{ QString{ u"José" } });
		CHECK(value(ExifDirectory::Ifd0, 0x013B, AsciiType, ascii("Jos\xE9")) == ExifValue{ QString{ u"José" } });
	}
	SECTION("UNDEFINED: printable ASCII is text, anything else bytes")
	{
		CHECK(value(ExifDirectory::Exif, 0x9000, UndefinedType, "0232") == ExifValue{ QString{ "0232" } }); // ExifVersion
		CHECK(value(ExifDirectory::Exif, 0x927C, UndefinedType, QByteArray{ "AB\x01", 3 }) == ExifValue{ QByteArray{ "AB\x01", 3 } });
	}
	SECTION("UserComment and GPS text: by their character code")
	{
		CHECK(value(ExifDirectory::Exif, 0x9286, UndefinedType, QByteArray{ "ASCII\0\0\0Hello", 13 }) == ExifValue{ QString{ "Hello" } });
		CHECK(value(ExifDirectory::Exif, 0x9286, UndefinedType, QByteArray(8, '\0') + "Hi  ") == ExifValue{ QString{ "Hi" } });
		CHECK(value(ExifDirectory::Exif, 0x9286, UndefinedType, QByteArray{ "UNICODE\0\0H\0i", 12 }, false) == ExifValue{ QString{ "Hi" } });
		CHECK(value(ExifDirectory::Gps, 0x1B, UndefinedType, QByteArray{ "ASCII\0\0\0GPS", 11 }) == ExifValue{ QString{ "GPS" } });
		const QByteArray jis = QByteArray{ "JIS\0\0\0\0\0", 8 } + "\x1B$B";
		CHECK(value(ExifDirectory::Exif, 0x9286, UndefinedType, jis) == ExifValue{ jis });
	}
	SECTION("XP tags: UTF-16LE in any byte order")
	{
		CHECK(value(ExifDirectory::Ifd0, 0x9C9B, ByteType, QByteArray{ "T\0i\0t\0\0\0", 8 }, false) == ExifValue{ QString{ "Tit" } });
	}
}

TEST_CASE("takeCaptureMetadata: decodes its fields and leaves the other entries", "[exif]")
{
	for (const ByteOrder order : { ByteOrder::Little, ByteOrder::Big })
	{
		const QByteArray tiff = tiffWithIfds(order,
			{
				{ MakeTag, AsciiType, 6, ascii("Canon") },
				{ 0x0110, AsciiType, 13, ascii("Canon EOS R5") },
				{ OrientationTag, ShortType, 1, bytesOf<uint16_t>(6, order) },
				{ 0x8298, AsciiType, 4, ascii("Me!") }, // Copyright
			},
			{
				{ 0x829A, RationalType, 1, rational(1, 250, order) },                                // ExposureTime
				{ 0x829D, RationalType, 1, rational(28, 10, order) },                               // FNumber
				{ 0x8827, ShortType, 1, bytesOf<uint16_t>(400, order) },                            // PhotographicSensitivity
				{ 0x9204, SRationalType, 1, bytesOf<int32_t>(-2, order) + bytesOf<int32_t>(3, order) }, // ExposureBiasValue
				{ 0x920A, RationalType, 1, rational(50, 1, order) },                                // FocalLength
				{ 0xA405, ShortType, 1, bytesOf<uint16_t>(0, order) },                              // FocalLengthIn35mmFilm: unknown
				{ 0x9207, ShortType, 1, bytesOf<uint16_t>(5, order) },                              // MeteringMode
				{ 0x9209, ShortType, 1, bytesOf<uint16_t>(0x10, order) },                           // Flash
				{ 0xA434, AsciiType, 7, ascii("RF50mm") },                                          // LensModel
				{ 0xA432, RationalType, 4, rational(24, 1, order) + rational(70, 1, order) + rational(4, 1, order) + rational(0, 0, order) }, // LensSpecification
				{ 0x9003, AsciiType, 20, ascii("2024:05:06 07:08:09") },                            // DateTimeOriginal
				{ 0x9291, AsciiType, 3, ascii("05") },                                              // SubSecTimeOriginal
				{ 0x9011, AsciiType, 7, ascii("-03:30") },                                          // OffsetTimeOriginal
				{ MakerNoteTag, UndefinedType, 9, QByteArray{ "MakerNote", 9 } },
			},
			{
				{ 0x0000, ByteType, 4, QByteArray{ "\x02\x03\x00\x00", 4 } }, // GPSVersionID
				{ 0x0001, AsciiType, 2, ascii("S") },
				{ 0x0002, RationalType, 3, rational(33, 1, order) + rational(51, 1, order) + rational(3000, 100, order) },
				{ 0x0003, AsciiType, 2, ascii("E") },
				{ 0x0004, RationalType, 3, rational(151, 1, order) + rational(12, 1, order) + rational(0, 1, order) },
				{ 0x0005, ByteType, 1, QByteArray{ "\x01", 1 } }, // below sea level
				{ 0x0006, RationalType, 1, rational(50, 1, order) },
			});
		ExifTags tags = exifTagsFromTiff(tiff);
		const CaptureMetadata metadata = takeCaptureMetadata(tags);

		CHECK(metadata.make == "Canon");
		CHECK(metadata.model == "Canon EOS R5");
		CHECK(metadata.copyright == "Me!");
		CHECK(metadata.lens == "RF50mm");
		CHECK(metadata.lensSpecification == LensSpecification{ 24.0, 70.0, 4.0, std::nullopt }); // 0/0: unknown
		CHECK(metadata.orientation == 6);
		CHECK(metadata.exposureTime == ExifRational{ 1, 250 });
		CHECK(metadata.fNumber == Approx(2.8));
		CHECK(metadata.iso == 400u);
		CHECK(metadata.exposureBias == ExifRational{ -2, 3 });
		CHECK(metadata.focalLength == Approx(50.0));
		CHECK_FALSE(metadata.focalLength35mm);
		CHECK(metadata.meteringMode == 5);
		CHECK(metadata.flash == 0x10);
		CHECK_FALSE(metadata.whiteBalance);
		CHECK(metadata.captureTime == CaptureTime{ QDate{ 2024, 5, 6 }, QTime{ 7, 8, 9, 50 }, -(3 * 3600 + 30 * 60) });
		REQUIRE(metadata.gps);
		CHECK(metadata.gps->latitude == Approx(-(33 + 51 / 60.0 + 30 / 3600.0)));
		CHECK(metadata.gps->longitude == Approx(151.2));
		CHECK(metadata.gps->altitude == Approx(-50.0));

		REQUIRE(tags.entries.size() == 2);
		CHECK(findEntry(tags, ExifDirectory::Exif, MakerNoteTag));
		CHECK(findEntry(tags, ExifDirectory::Gps, 0x0000));
	}
}

TEST_CASE("takeCaptureMetadata: entries that fail to decode stay", "[exif]")
{
	constexpr ByteOrder order = ByteOrder::Little;
	const QByteArray tiff = tiffWithIfds(order, {},
		{
			{ 0x829A, RationalType, 1, rational(1, 0, order) },             // ExposureTime: no denominator
			{ 0x829D, ShortType, 1, bytesOf<uint16_t>(4, order) },          // FNumber: not a RATIONAL
			{ 0x9003, AsciiType, 20, ascii("0000:00:00 00:00:00") },        // DateTimeOriginal: unset
			{ 0x9011, AsciiType, 7, ascii("+01:00") },                      // OffsetTimeOriginal
		},
		{
			{ 0x0002, RationalType, 3, rational(33, 1, order) + rational(51, 1, order) + rational(0, 1, order) }, // a latitude without a longitude
		});
	ExifTags tags = exifTagsFromTiff(tiff);
	const ExifTags read = tags;
	const CaptureMetadata metadata = takeCaptureMetadata(tags);
	CHECK(metadata == CaptureMetadata{});
	CHECK(tags == read);
}

TEST_CASE("takeCaptureMetadata, the lens specification: DNG's LensInfo when LensSpecification is missing or invalid", "[exif]")
{
	constexpr ByteOrder order = ByteOrder::Little;
	const QByteArray lensInfo = rational(90, 1, order) + rational(90, 1, order) + rational(28, 10, order) + rational(28, 10, order);
	const QByteArray noFocalRange = rational(0, 1, order) + rational(70, 1, order) + rational(0, 0, order) + rational(0, 0, order);
	ExifTags tags = exifTagsFromTiff(tiffWithIfds(order, { { 0xC630, RationalType, 4, lensInfo } }, { { 0xA432, RationalType, 4, noFocalRange } }));
	const CaptureMetadata metadata = takeCaptureMetadata(tags);
	CHECK(metadata.lensSpecification == LensSpecification{ 90.0, 90.0, 2.8, 2.8 });
	REQUIRE(tags.entries.size() == 1);
	CHECK(findEntry(tags, ExifDirectory::Exif, 0xA432));
}

TEST_CASE("The names of EXIF codes", "[exif]")
{
	CHECK(meteringModeName(5) == "Pattern");
	CHECK(meteringModeName(7).empty());
	CHECK(exposureProgramName(3) == "Aperture priority");
	CHECK(whiteBalanceName(1) == "Manual");
	CHECK(colorSpaceName(0xFFFF) == "Uncalibrated");
	CHECK(orientationName(6) == "Rotate 90° clockwise");
	CHECK(orientationName(0).empty());

	CHECK(flashDescription(0x00) == "Did not fire");
	CHECK(flashDescription(0x10) == "Did not fire, suppressed");
	CHECK(flashDescription(0x19) == "Fired, auto mode");
	CHECK(flashDescription(0x5F) == "Fired, auto mode, return light detected, red-eye reduction");
	CHECK(flashDescription(0x20) == "No flash function");
}

TEST_CASE("readImageInfo: a JPEG's EXIF, comments and upright size; a PNG's text chunks, EXIF and color profile", "[exif]")
{
	QTemporaryDir folder;
	REQUIRE(folder.isValid());

	QImage image{ 3, 2, QImage::Format_RGB32 };
	image.fill(Qt::gray);
	image.setText("Title", "Sunset");

	SECTION("JPEG")
	{
		QBuffer encoded;
		REQUIRE(image.save(&encoded, "JPG"));
		const QByteArray tiff = tiffWithIfds(ByteOrder::Big, { { MakeTag, AsciiType, 6, ascii("Canon") }, { OrientationTag, ShortType, 1, bytesOf<uint16_t>(6, ByteOrder::Big) } },
			{ { MakerNoteTag, UndefinedType, 9, QByteArray{ "MakerNote", 9 } } });
		const QString path = folder.filePath("photo.jpg");
		QFile file{ path };
		REQUIRE(file.open(QIODevice::WriteOnly));
		const QByteArray bytes = jpegWithExifApp1(encoded.data(), exifPayload(tiff));
		REQUIRE(file.write(bytes) == bytes.size());
		file.close();

		const ImageInfo info = readImageInfo(path);
		CHECK(info.error.isEmpty());
		CHECK(info.fileSize == bytes.size());
		CHECK(info.lastModified.isValid());
		CHECK(info.format == "jpeg");
		CHECK(info.uprightSize == QSize{ 2, 3 });
		CHECK_FALSE(info.colorProfile);
		CHECK(std::ranges::contains(info.texts, std::pair{ QString{ "Title" }, QString{ "Sunset" } }));
		CHECK(info.capture.make == "Canon");
		CHECK(info.capture.orientation == 6);
		REQUIRE(info.otherExif.entries.size() == 1);
		CHECK(info.otherExif.entries.front().tag == MakerNoteTag);
	}

	SECTION("PNG")
	{
		image.setColorSpace(QColorSpace::DisplayP3);
		QBuffer encoded;
		REQUIRE(image.save(&encoded, "PNG"));
		QByteArray png = encoded.data();
		png.insert(8 + 25, pngChunk("eXIf", exifTiff(ByteOrder::Little, { { OrientationTag, 1 } }))); // after the signature and IHDR
		const QString path = folder.filePath("image.png");
		QFile file{ path };
		REQUIRE(file.open(QIODevice::WriteOnly));
		REQUIRE(file.write(png) == png.size());
		file.close();

		const ImageInfo info = readImageInfo(path);
		CHECK(info.format == "png");
		CHECK(info.uprightSize == QSize{ 3, 2 });
		REQUIRE(info.colorProfile);
		CHECK(info.colorProfile->source == ColorProfile::Source::Embedded);
		REQUIRE(info.colorProfile->name);
		CHECK_FALSE(info.colorProfile->name->isEmpty());
		CHECK(std::ranges::contains(info.texts, std::pair{ QString{ "Title" }, QString{ "Sunset" } }));
		CHECK(info.capture.orientation == 1);
	}
}

TEST_CASE("readImageInfo, the color profile: Adobe RGB a JPEG's EXIF declares, unless one is embedded; InterColorProfile not also a tag", "[exif]")
{
	QTemporaryDir folder;
	REQUIRE(folder.isValid());
	const auto written = [&](const QString& name, const QByteArray& bytes) {
		const QString path = folder.filePath(name);
		QFile file{ path };
		REQUIRE(file.open(QIODevice::WriteOnly));
		REQUIRE(file.write(bytes) == bytes.size());
		return path;
	};

	QImage image{ 3, 2, QImage::Format_RGB32 };
	image.fill(Qt::gray);
	QBuffer encoded;
	REQUIRE(image.save(&encoded, "JPG"));
	// Uncalibrated, with the interoperability index R03
	const QByteArray tiff = tiffWithIfds(ByteOrder::Little, { { MakeTag, AsciiType, 6, ascii("Canon") } },
		{ { 0xA001, ShortType, 1, bytesOf<uint16_t>(0xFFFF, ByteOrder::Little) } }, {}, { { 0x0001, AsciiType, 4, ascii("R03") } });
	const QByteArray declaring = jpegWithExifApp1(encoded.data(), exifPayload(tiff));

	const ImageInfo declared = readImageInfo(written("declared.jpg", declaring));
	REQUIRE(declared.colorProfile);
	CHECK(declared.colorProfile->source == ColorProfile::Source::ExifAdobeRgb);
	CHECK(declared.colorProfile->name == "Adobe RGB (1998)");

	QByteArray embedding = declaring;
	embedding.insert(2, iccSegment(1, 1, QColorSpace{ QColorSpace::SRgb }.iccProfile()));
	const ImageInfo embedded = readImageInfo(written("embedded.jpg", embedding));
	REQUIRE(embedded.colorProfile);
	CHECK(embedded.colorProfile->source == ColorProfile::Source::Embedded);

	const QByteArray profile{ "profile data" };
	const ImageInfo tiffInfo = readImageInfo(written("image.tif", tiffWithIfds(ByteOrder::Little, { { MakeTag, AsciiType, 6, ascii("Canon") }, { 0x8773, UndefinedType, 12, profile } })));
	REQUIRE(tiffInfo.colorProfile);
	CHECK(tiffInfo.colorProfile->source == ColorProfile::Source::Embedded);
	CHECK_FALSE(tiffInfo.colorProfile->name); // not a profile Little CMS reads
	CHECK(tiffInfo.capture.make == "Canon");
	CHECK(tiffInfo.otherExif.entries.empty());
}

TEST_CASE("readImageInfo: an error, and nothing else, for a file that cannot be read", "[exif]")
{
	const ImageInfo info = readImageInfo(QDir::temp().filePath("no such file.jpg"));
	CHECK_FALSE(info.error.isEmpty());
	CHECK(info.fileSize == 0);
	CHECK(info.format.isEmpty());
}
