#include "ImageMetadata/Exif.h"
#include "assert/advanced_assert.h"

DISABLE_COMPILER_WARNINGS
#include <QtEndian>
#include <QtGlobal>
#include <libraw/libraw.h>
RESTORE_COMPILER_WARNINGS

#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace ImageMetadata {

namespace {

// EXIF orientation values 1-8, mapped as Qt's own JPEG plugin maps them
QImageIOHandler::Transformations transformationsForExifOrientation(uint16_t orientation) noexcept
{
	switch (orientation)
	{
	case 2: return QImageIOHandler::TransformationMirror;
	case 3: return QImageIOHandler::TransformationRotate180;
	case 4: return QImageIOHandler::TransformationFlip;
	case 5: return QImageIOHandler::TransformationFlipAndRotate90;
	case 6: return QImageIOHandler::TransformationRotate90;
	case 7: return QImageIOHandler::TransformationMirrorAndRotate90;
	case 8: return QImageIOHandler::TransformationRotate270;
	default: return QImageIOHandler::TransformationNone;
	}
}

constexpr uint16_t MakeTag = 0x010F;
constexpr uint16_t OrientationTag = 0x0112;
constexpr uint16_t ExifIfdTag = 0x8769;
constexpr uint16_t GpsIfdTag = 0x8825;
constexpr uint16_t MakerNoteTag = 0x927C;
constexpr uint16_t ColorSpaceTag = 0xA001;
constexpr uint16_t PixelXDimensionTag = 0xA002;
constexpr uint16_t PixelYDimensionTag = 0xA003;
constexpr uint16_t InteroperabilityIfdTag = 0xA005;
constexpr uint16_t InteroperabilityIndexTag = 0x0001;

constexpr uint16_t ShortType = 3;
constexpr uint16_t LongType = 4;
constexpr uint16_t RationalType = 5;
constexpr uint16_t SignedRationalType = 10;

constexpr QByteArrayView ExifHeader{ "Exif\0\0", 6 };
// An APP1 segment's length field counts its own two bytes and holds at most 65535
constexpr qsizetype MaxApp1PayloadSize = 65533;

// The size of one value of a TIFF field type; 0 for an unknown type
int typeSize(uint16_t type) noexcept
{
	// BYTE, ASCII, SHORT, LONG, RATIONAL, SBYTE, UNDEFINED, SSHORT, SLONG, SRATIONAL, FLOAT, DOUBLE, IFD
	constexpr std::array<int, 14> sizes{ 0, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8, 4 };
	return type < sizes.size() ? sizes[type] : 0;
}

// The sub-IFD a pointer entry of directory leads to; empty when tag is not one of directory's pointers
std::optional<ExifDirectory> pointedDirectory(ExifDirectory directory, uint16_t tag) noexcept
{
	if (directory == ExifDirectory::Ifd0 && tag == ExifIfdTag)
		return ExifDirectory::Exif;
	if (directory == ExifDirectory::Ifd0 && tag == GpsIfdTag)
		return ExifDirectory::Gps;
	if (directory == ExifDirectory::Exif && tag == InteroperabilityIfdTag)
		return ExifDirectory::Interoperability;
	return std::nullopt;
}

template <typename T>
void store(char* dest, T value, bool littleEndian) noexcept
{
	if (littleEndian)
		qToLittleEndian(value, dest);
	else
		qToBigEndian(value, dest);
}

template <typename T>
QByteArray encoded(T value, bool littleEndian)
{
	QByteArray bytes(sizeof(T), Qt::Uninitialized);
	store(bytes.data(), value, littleEndian);
	return bytes;
}

// EXIF is a TIFF structure: a byte order mark, 42, then IFD0's offset.
// An IFD is an entry count, 12-byte entries, then the next IFD's offset. IFD0's next IFD is IFD1, the thumbnail's.
// An entry is a tag, a type, a count, then a 4-byte field holding the value when it fits, else the value's offset.
struct TiffView
{
	QByteArrayView bytes;
	bool littleEndian = true;

	[[nodiscard]] uint16_t read16(qsizetype offset) const noexcept
	{
		return littleEndian ? qFromLittleEndian<uint16_t>(bytes.data() + offset) : qFromBigEndian<uint16_t>(bytes.data() + offset);
	}

	[[nodiscard]] uint32_t read32(qsizetype offset) const noexcept
	{
		return littleEndian ? qFromLittleEndian<uint32_t>(bytes.data() + offset) : qFromBigEndian<uint32_t>(bytes.data() + offset);
	}

	// The offset of the next-IFD field of the IFD at ifd; empty unless the whole IFD lies within bytes
	[[nodiscard]] std::optional<qsizetype> nextIfdField(qsizetype ifd) const noexcept
	{
		if (ifd < 8 || ifd + 2 > bytes.size())
			return std::nullopt;

		const qsizetype field = ifd + 2 + qsizetype{ read16(ifd) } * 12;
		return field + 4 <= bytes.size() ? std::optional{ field } : std::nullopt;
	}
};

// Empty unless tiff starts with a TIFF header
std::optional<TiffView> tiffView(QByteArrayView tiff) noexcept
{
	if (tiff.size() < 8)
		return std::nullopt;

	const bool littleEndian = tiff.startsWith("II");
	if (!littleEndian && !tiff.startsWith("MM"))
		return std::nullopt;

	const TiffView view{ tiff, littleEndian };
	return view.read16(2) == 42 ? std::optional{ view } : std::nullopt;
}

// An entry of a TIFF structure, its value lying within the data
struct LocatedEntry
{
	ExifDirectory directory;
	uint16_t tag;
	uint16_t type;
	uint32_t count;
	qsizetype valueOffset;
	qsizetype valueSize;
};

// Calls visit for each entry of the IFD at ifd, then for the entries of the IFDs it leads to. Pointers are followed, not visited.
// Leaves out entries of an unknown type or with a value outside the data; an IFD cut short ends at its last whole entry.
template <typename Visitor>
void visitIfd(const TiffView& tiff, ExifDirectory directory, qsizetype ifd, const Visitor& visit)
{
	const qsizetype size = tiff.bytes.size();
	if (ifd < 8 || ifd + 2 > size)
		return;

	const int entryCount = tiff.read16(ifd);
	for (int i = 0; i < entryCount; ++i)
	{
		const qsizetype entry = ifd + 2 + qsizetype{ i } * 12;
		if (entry + 12 > size)
			return;

		const uint16_t tag = tiff.read16(entry), type = tiff.read16(entry + 2);
		const uint32_t count = tiff.read32(entry + 4);
		if (const std::optional<ExifDirectory> pointed = pointedDirectory(directory, tag))
		{
			visitIfd(tiff, *pointed, tiff.read32(entry + 8), visit);
			continue;
		}

		const uint64_t valueSize = uint64_t{ count } * static_cast<uint64_t>(typeSize(type));
		const qsizetype valueOffset = valueSize <= 4 ? entry + 8 : tiff.read32(entry + 8);
		if (typeSize(type) == 0 || valueOffset > size || valueSize > static_cast<uint64_t>(size - valueOffset))
			continue;

		visit(LocatedEntry{ directory, tag, type, count, valueOffset, static_cast<qsizetype>(valueSize) });
	}

	if (directory == ExifDirectory::Ifd0)
	{
		if (const std::optional<qsizetype> next = tiff.nextIfdField(ifd))
			visitIfd(tiff, ExifDirectory::Ifd1, tiff.read32(*next), visit);
	}
}

template <typename Visitor>
void forEachEntry(const TiffView& tiff, const Visitor& visit)
{
	visitIfd(tiff, ExifDirectory::Ifd0, tiff.read32(4), visit);
}

ExifMetadata metadataFromExifTiff(QByteArrayView bytes) noexcept
{
	const std::optional<TiffView> tiff = tiffView(bytes);
	if (!tiff)
		return {};

	ExifMetadata metadata;
	std::optional<uint16_t> colorSpace;
	bool interoperabilityIndexR03 = false;
	forEachEntry(*tiff, [&](const LocatedEntry& entry) {
		const bool isShort = entry.type == ShortType && entry.count >= 1;
		if (entry.directory == ExifDirectory::Ifd0 && entry.tag == OrientationTag && isShort)
			metadata.orientation = transformationsForExifOrientation(tiff->read16(entry.valueOffset));
		else if (entry.directory == ExifDirectory::Exif && entry.tag == ColorSpaceTag && isShort)
			colorSpace = tiff->read16(entry.valueOffset);
		else if (entry.directory == ExifDirectory::Interoperability && entry.tag == InteroperabilityIndexTag)
			interoperabilityIndexR03 = tiff->bytes.sliced(entry.valueOffset, entry.valueSize).startsWith("R03");
	});
	metadata.adobeRgb = colorSpace == 2 || (colorSpace == 0xFFFF && interoperabilityIndexR03);
	return metadata;
}

// payload as findExifApp1() returns it, edited as exifApp1ForOutput() describes
QByteArray patchedExifApp1(QByteArrayView payload, QSize uprightOutputSize)
{
	const std::optional<TiffView> tiff = tiffView(payload.sliced(ExifHeader.size()));
	if (!tiff)
		return {};

	QByteArray patched = payload.toByteArray();
	char* const patchedTiff = patched.data() + ExifHeader.size();
	bool upright = true;
	forEachEntry(*tiff, [&](const LocatedEntry& entry) {
		if (entry.count != 1)
			return;

		if (entry.directory == ExifDirectory::Ifd0 && entry.tag == OrientationTag && entry.type == ShortType)
		{
			upright = tiff->read16(entry.valueOffset) == 1;
			store<uint16_t>(patchedTiff + entry.valueOffset, 1, tiff->littleEndian);
		}
		else if (entry.directory == ExifDirectory::Exif && (entry.tag == PixelXDimensionTag || entry.tag == PixelYDimensionTag))
		{
			const int dimension = entry.tag == PixelXDimensionTag ? uprightOutputSize.width() : uprightOutputSize.height();
			if (entry.type == LongType)
				store(patchedTiff + entry.valueOffset, static_cast<uint32_t>(dimension), tiff->littleEndian);
			else if (entry.type == ShortType && dimension <= 0xFFFF)
				store(patchedTiff + entry.valueOffset, static_cast<uint16_t>(dimension), tiff->littleEndian);
		}
	});

	// IFD1's thumbnail keeps the source's orientation
	if (!upright)
	{
		if (const std::optional<qsizetype> next = tiff->nextIfdField(tiff->read32(4)))
			store<uint32_t>(patchedTiff + *next, 0, tiff->littleEndian);
	}
	return patched;
}

// Where a tag LibRaw's exif_cb reports was found, which LibRaw marks in the tag's upper bits
struct LibRawTagSource
{
	ExifDirectory directory;
	// Set for a TIFF IFD, a candidate for IFD0: its index in LibRaw's parsing order, -1 for every IFD of a Panasonic raw
	std::optional<int> tiffIfd;
};

std::optional<LibRawTagSource> libRawTagSource(int markedTag) noexcept
{
	const int marks = markedTag >> 16;
	switch (marks)
	{
	case 0x00: return LibRawTagSource{ ExifDirectory::Exif };
	case 0x03: return LibRawTagSource{ ExifDirectory::Ifd0, -1 };
	case 0x04: return LibRawTagSource{ ExifDirectory::Interoperability };
	case 0x05: return LibRawTagSource{ ExifDirectory::Gps };
	default: break;
	}
	// A TIFF IFD is marked (index + 1) << 20; the other marks are makers' own structures
	if (marks >= 0x10 && (marks & 0xF) == 0)
		return LibRawTagSource{ ExifDirectory::Ifd0, (marks >> 4) - 1 };
	return std::nullopt;
}

// What LibRaw's exif_cb reports while LibRaw opens a raw
struct LibRawExifCollector
{
	ExifTags tags; // without IFD0, which exifTagsFromRaw() picks from tiffIfds
	std::map<int, std::vector<ExifEntry>> tiffIfds; // by LibRawTagSource::tiffIfd
	bool byteOrderKnown = false;
};

// Reverses the byte order of each value; a RATIONAL is two LONGs
void reverseByteOrder(QByteArray& value, uint16_t type)
{
	const int unit = type == RationalType || type == SignedRationalType ? 4 : typeSize(type);
	assert_and_return_r(unit > 0, );
	for (qsizetype i = 0; i + unit <= value.size(); i += unit)
		std::reverse(value.begin() + i, value.begin() + i + unit);
}

// LibRaw's exif_cb: adds the entry to the LibRawExifCollector at context. stream is positioned at the value.
void collectLibRawExifEntry(void* context, int markedTag, int type, int count, unsigned int byteOrder, void* stream, INT64 /*base*/)
{
	LibRawExifCollector& collector = *static_cast<LibRawExifCollector*>(context);
	const std::optional<LibRawTagSource> source = libRawTagSource(markedTag);
	const auto tag = static_cast<uint16_t>(markedTag & 0xFFFF);
	const auto fieldType = static_cast<uint16_t>(type);
	if (!source || pointedDirectory(source->directory, tag) || typeSize(fieldType) == 0 || count < 0)
		return;

	std::vector<ExifEntry>& entries = source->tiffIfd ? collector.tiffIfds[*source->tiffIfd] : collector.tags.entries;
	if (std::ranges::any_of(entries, [&](const ExifEntry& entry) { return entry.directory == source->directory && entry.tag == tag; }))
		return;

	auto& input = *static_cast<LibRaw_abstract_datastream*>(stream);
	const uint64_t valueSize = static_cast<uint64_t>(count) * static_cast<uint64_t>(typeSize(fieldType));
	if (valueSize > static_cast<uint64_t>(std::max<INT64>(input.size() - input.tell(), 0)))
		return;

	QByteArray value(static_cast<qsizetype>(valueSize), Qt::Uninitialized);
	if (input.read(value.data(), 1, static_cast<size_t>(value.size())) != value.size())
		return;

	const bool littleEndian = byteOrder == 0x4949; // "II"
	if (!collector.byteOrderKnown)
	{
		collector.tags.littleEndian = littleEndian;
		collector.byteOrderKnown = true;
	}
	else if (littleEndian != collector.tags.littleEndian)
	{
		reverseByteOrder(value, fieldType);
	}
	entries.push_back({ source->directory, tag, fieldType, static_cast<uint32_t>(count), std::move(value) });
}

} // namespace

ExifMetadata readJpegExif(QByteArrayView jpeg) noexcept
{
	const std::optional<QByteArrayView> payload = findExifApp1(jpeg);
	return payload ? readExifApp1(*payload).value_or(ExifMetadata{}) : ExifMetadata{};
}

std::optional<ExifMetadata> readExifApp1(QByteArrayView payload) noexcept
{
	if (!payload.startsWith(ExifHeader))
		return std::nullopt;
	return metadataFromExifTiff(payload.sliced(ExifHeader.size()));
}

std::optional<QByteArrayView> findExifApp1(QByteArrayView jpeg) noexcept
{
	// After SOI, each marker is FF, its code and a big-endian length that counts itself; the image data starts at SOS
	qsizetype pos = 2;
	while (pos + 4 <= jpeg.size() && static_cast<uchar>(jpeg[pos]) == 0xFF)
	{
		const uchar marker = static_cast<uchar>(jpeg[pos + 1]);
		if (marker == 0xFF) // fill byte
		{
			++pos;
			continue;
		}
		if (marker == 0xDA || marker == 0xD9) // SOS, EOI
			break;

		const qsizetype length = qFromBigEndian<uint16_t>(jpeg.data() + pos + 2);
		if (length < 2 || pos + 2 + length > jpeg.size())
			break;

		if (marker == 0xE1) // APP1
		{
			if (const QByteArrayView payload = jpeg.sliced(pos + 4, length - 2); payload.startsWith(ExifHeader))
				return payload;
		}

		pos += 2 + length;
	}
	return std::nullopt;
}

ExifTags exifTagsFromApp1(QByteArrayView payload)
{
	if (!payload.startsWith(ExifHeader))
		return {};
	const std::optional<TiffView> tiff = tiffView(payload.sliced(ExifHeader.size()));
	if (!tiff)
		return {};

	ExifTags tags{ .littleEndian = tiff->littleEndian };
	forEachEntry(*tiff, [&](const LocatedEntry& entry) {
		tags.entries.push_back({ entry.directory, entry.tag, entry.type, entry.count, tiff->bytes.sliced(entry.valueOffset, entry.valueSize).toByteArray() });
	});
	return tags;
}

ExifTags exifTagsFromRaw(QByteArrayView rawFile)
{
	LibRawExifCollector collector;
	// Heap-allocated: a LibRaw instance is hundreds of KB
	const auto raw = std::make_unique<LibRaw>();
	raw->set_exifparser_handler(&collectLibRawExifEntry, &collector);
	if (raw->open_buffer(rawFile.data(), static_cast<size_t>(rawFile.size())) != LIBRAW_SUCCESS)
		return {};

	// IFD0 is the first TIFF IFD holding a Make, else the first: a Fujifilm raw's comes from its embedded JPEG, after the raw data's IFD
	if (collector.tiffIfds.empty())
		return std::move(collector.tags);
	auto ifd0 = std::ranges::find_if(collector.tiffIfds, [](const auto& ifd) { return std::ranges::contains(ifd.second, MakeTag, &ExifEntry::tag); });
	if (ifd0 == collector.tiffIfds.end())
		ifd0 = collector.tiffIfds.begin();

	std::vector<ExifEntry>& entries = collector.tags.entries;
	entries.insert(entries.begin(), std::make_move_iterator(ifd0->second.begin()), std::make_move_iterator(ifd0->second.end()));
	return std::move(collector.tags);
}

void prepareExifTagsForOutput(ExifTags& tags, QSize uprightOutputSize)
{
	// static: see writtenDirectories in exifApp1FromTags()
	static constexpr std::array<uint16_t, 18> descriptiveIfd0Tags{
		0x010E, // ImageDescription
		MakeTag,
		0x0110, // Model
		OrientationTag,
		0x011A, // XResolution
		0x011B, // YResolution
		0x0128, // ResolutionUnit
		0x0131, // Software
		0x0132, // DateTime
		0x013B, // Artist
		0x4746, // Rating
		0x4749, // RatingPercent
		0x8298, // Copyright
		0x9C9B, // XPTitle
		0x9C9C, // XPComment
		0x9C9D, // XPAuthor
		0x9C9E, // XPKeywords
		0x9C9F, // XPSubject
	};
	std::erase_if(tags.entries, [&](const ExifEntry& entry) {
		switch (entry.directory)
		{
		case ExifDirectory::Ifd0: return !std::ranges::contains(descriptiveIfd0Tags, entry.tag);
		case ExifDirectory::Exif: return entry.tag == MakerNoteTag;
		case ExifDirectory::Ifd1: return true;
		default: return false;
		}
	});

	for (ExifEntry& entry : tags.entries)
	{
		if (entry.directory == ExifDirectory::Ifd0 && entry.tag == OrientationTag)
			entry = { ExifDirectory::Ifd0, OrientationTag, ShortType, 1, encoded<uint16_t>(1, tags.littleEndian) };
	}

	if (!std::ranges::any_of(tags.entries, [](const ExifEntry& entry) { return entry.directory == ExifDirectory::Exif; }))
		return;

	const auto setPixelDimension = [&](uint16_t tag, int dimension) {
		ExifEntry entry{ ExifDirectory::Exif, tag, LongType, 1, encoded(static_cast<uint32_t>(dimension), tags.littleEndian) };
		const auto existing = std::ranges::find_if(tags.entries, [&](const ExifEntry& e) { return e.directory == ExifDirectory::Exif && e.tag == tag; });
		if (existing != tags.entries.end())
			*existing = std::move(entry);
		else
			tags.entries.push_back(std::move(entry));
	};
	setPixelDimension(PixelXDimensionTag, uprightOutputSize.width());
	setPixelDimension(PixelYDimensionTag, uprightOutputSize.height());
}

QByteArray exifApp1FromTags(const ExifTags& tags)
{
	// Written in this order, each IFD followed by the values that do not fit its entries.
	// static: MSVC's optimized build crashes when a lambda captures this constexpr local by reference.
	static constexpr std::array writtenDirectories{ ExifDirectory::Ifd0, ExifDirectory::Exif, ExifDirectory::Gps, ExifDirectory::Interoperability };
	std::array<std::vector<ExifEntry>, writtenDirectories.size()> ifds;
	const auto indexOf = [&](ExifDirectory directory) {
		return static_cast<size_t>(std::ranges::find(writtenDirectories, directory) - writtenDirectories.begin());
	};

	for (const ExifEntry& entry : tags.entries)
	{
		if (entry.directory == ExifDirectory::Ifd1)
			continue;

		const bool consistent = typeSize(entry.type) != 0 && entry.value.size() == qsizetype{ entry.count } * typeSize(entry.type);
		assert_r(consistent);
		if (consistent)
			ifds[indexOf(entry.directory)].push_back(entry);
	}

	// Each pointer's value is set once the layout is known
	const auto addPointer = [&](ExifDirectory from, uint16_t tag) {
		ifds[indexOf(from)].push_back({ from, tag, LongType, 1, QByteArray(4, '\0') });
	};
	if (!ifds[indexOf(ExifDirectory::Interoperability)].empty())
		addPointer(ExifDirectory::Exif, InteroperabilityIfdTag);
	if (!ifds[indexOf(ExifDirectory::Exif)].empty())
		addPointer(ExifDirectory::Ifd0, ExifIfdTag);
	if (!ifds[indexOf(ExifDirectory::Gps)].empty())
		addPointer(ExifDirectory::Ifd0, GpsIfdTag);
	if (ifds[indexOf(ExifDirectory::Ifd0)].empty())
		return {};

	// TIFF requires ascending tags; an out-of-line value starts on a word boundary
	const auto outOfLineSize = [](const ExifEntry& entry) { return entry.value.size() > 4 ? (entry.value.size() + 1) & ~qsizetype{ 1 } : 0; };
	std::array<qsizetype, writtenDirectories.size()> ifdOffsets{};
	qsizetype tiffSize = 8;
	for (size_t i = 0; i < ifds.size(); ++i)
	{
		if (ifds[i].empty())
			continue;

		std::ranges::stable_sort(ifds[i], {}, &ExifEntry::tag);
		ifdOffsets[i] = tiffSize;
		tiffSize += 2 + 12 * std::ssize(ifds[i]) + 4;
		for (const ExifEntry& entry : ifds[i])
			tiffSize += outOfLineSize(entry);
	}
	if (ExifHeader.size() + tiffSize > MaxApp1PayloadSize)
		return {};

	const bool littleEndian = tags.littleEndian;
	for (std::vector<ExifEntry>& ifd : ifds)
	{
		for (ExifEntry& entry : ifd)
		{
			if (const std::optional<ExifDirectory> pointed = pointedDirectory(entry.directory, entry.tag))
				store(entry.value.data(), static_cast<uint32_t>(ifdOffsets[indexOf(*pointed)]), littleEndian);
		}
	}

	QByteArray payload = ExifHeader.toByteArray();
	payload.reserve(ExifHeader.size() + tiffSize);
	payload += littleEndian ? "II" : "MM";
	payload += encoded<uint16_t>(42, littleEndian) + encoded<uint32_t>(8, littleEndian);
	for (size_t i = 0; i < ifds.size(); ++i)
	{
		if (ifds[i].empty())
			continue;

		assert_r(payload.size() - ExifHeader.size() == ifdOffsets[i]);
		const qsizetype valuesOffset = ifdOffsets[i] + 2 + 12 * std::ssize(ifds[i]) + 4;
		QByteArray values;
		payload += encoded(static_cast<uint16_t>(ifds[i].size()), littleEndian);
		for (const ExifEntry& entry : ifds[i])
		{
			payload += encoded(entry.tag, littleEndian) + encoded(entry.type, littleEndian) + encoded(entry.count, littleEndian);
			if (entry.value.size() <= 4)
			{
				payload += entry.value + QByteArray(4 - entry.value.size(), '\0');
				continue;
			}

			payload += encoded(static_cast<uint32_t>(valuesOffset + values.size()), littleEndian);
			values += entry.value;
			if (values.size() % 2 != 0)
				values += '\0';
		}
		payload += encoded<uint32_t>(0, littleEndian) + values; // no next IFD
	}
	return payload;
}

QByteArray exifApp1ForOutput(QByteArrayView sourceFile, QSize uprightOutputSize)
{
	if (sourceFile.startsWith("\xFF\xD8")) // SOI
	{
		const std::optional<QByteArrayView> payload = findExifApp1(sourceFile);
		return payload ? patchedExifApp1(*payload, uprightOutputSize) : QByteArray{};
	}

	ExifTags tags = exifTagsFromRaw(sourceFile);
	prepareExifTagsForOutput(tags, uprightOutputSize);
	return exifApp1FromTags(tags);
}

QByteArray jpegWithExifApp1(QByteArrayView jpeg, QByteArrayView exifApp1)
{
	assert_and_return_r(jpeg.startsWith("\xFF\xD8") && exifApp1.size() <= MaxApp1PayloadSize, jpeg.toByteArray());
	if (exifApp1.empty())
		return jpeg.toByteArray();

	// JFIF requires its APP0 right after SOI
	qsizetype insertAt = 2;
	if (jpeg.size() >= 6 && static_cast<uchar>(jpeg[2]) == 0xFF && static_cast<uchar>(jpeg[3]) == 0xE0)
	{
		if (const qsizetype app0End = 4 + qsizetype{ qFromBigEndian<uint16_t>(jpeg.data() + 4) }; app0End <= jpeg.size())
			insertAt = app0End;
	}

	QByteArray result;
	result.reserve(jpeg.size() + 4 + exifApp1.size());
	result += jpeg.first(insertAt);
	result += "\xFF\xE1";
	result += encoded(static_cast<uint16_t>(exifApp1.size() + 2), false);
	result += exifApp1;
	result += jpeg.sliced(insertAt);
	return result;
}

} // namespace ImageMetadata
