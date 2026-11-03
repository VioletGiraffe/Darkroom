#include "ImageDecoding/ImageDecoding.h"
#include "ImageMetadata/Exif.h"
#include "JpegTestData.h"

#include "3rdparty/catch2/catch.hpp"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSize>
#include <QString>
#include <QtEndian>

#include <algorithm>
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

constexpr uint16_t ByteType = 1, AsciiType = 2, ShortType = 3, LongType = 4, RationalType = 5, UndefinedType = 7;
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
	const QByteArray tiff = exifApp1FromTags(sampleTags(ByteOrder::Little)).sliced(6);
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
		const ExifEntry* makerNote = findEntry(tags, ExifDirectory::Exif, MakerNoteTag);
		WARN(QStringLiteral("%1: IFD0 %2, Exif %3, GPS %4, interoperability %5 entries; Make %6, DateTimeOriginal %7, MakerNote %8 bytes")
			.arg(file.fileName()).arg(entryCount(ExifDirectory::Ifd0)).arg(entryCount(ExifDirectory::Exif)).arg(entryCount(ExifDirectory::Gps))
			.arg(entryCount(ExifDirectory::Interoperability)).arg(text(ExifDirectory::Ifd0, MakeTag), text(ExifDirectory::Exif, 0x9003))
			.arg(makerNote ? makerNote->value.size() : 0).toStdString());
		CHECK(findEntry(tags, ExifDirectory::Ifd0, MakeTag));
		CHECK(findEntry(tags, ExifDirectory::Exif, 0x9003)); // DateTimeOriginal

		const QSize outputSize{ 1200, 800 };
		ExifTags expected = tags;
		prepareExifTagsForOutput(expected, outputSize);
		const QByteArray payload = exifApp1ForOutput(bytes, outputSize);
		CHECK_FALSE(payload.isEmpty());
		CHECK(sortedEntries(exifTagsFromApp1(payload).entries) == sortedEntries(expected.entries));
	}
}
