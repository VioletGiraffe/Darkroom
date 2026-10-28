#include "ImageDecoding/Exif.h"

DISABLE_COMPILER_WARNINGS
#include <QtEndian>
#include <QtGlobal>
RESTORE_COMPILER_WARNINGS

#include <cstdint>
#include <optional>

namespace ImageDecoding {

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

constexpr uint16_t OrientationTag = 0x0112;
constexpr uint16_t ExifIfdTag = 0x8769;
constexpr uint16_t ColorSpaceTag = 0xA001;
constexpr uint16_t InteroperabilityIfdTag = 0xA005;
constexpr uint16_t InteroperabilityIndexTag = 0x0001;

// EXIF is a TIFF structure: a byte order mark, 42, then IFD0's offset.
// An IFD is an entry count and 12-byte entries: tag, type, count and a 4-byte value field.
// The value field holds a SHORT in its first two bytes, a LONG, or up to 4 ASCII bytes; a LONG here is a sub-IFD's offset.
ExifMetadata metadataFromExifTiff(QByteArrayView tiff) noexcept
{
	if (tiff.size() < 8)
		return {};

	const bool littleEndian = tiff.startsWith("II");
	if (!littleEndian && !tiff.startsWith("MM"))
		return {};

	const auto read16 = [&](qsizetype offset) {
		return littleEndian ? qFromLittleEndian<uint16_t>(tiff.data() + offset) : qFromBigEndian<uint16_t>(tiff.data() + offset);
	};
	const auto read32 = [&](qsizetype offset) {
		return littleEndian ? qFromLittleEndian<uint32_t>(tiff.data() + offset) : qFromBigEndian<uint32_t>(tiff.data() + offset);
	};

	if (read16(2) != 42)
		return {};

	// The offset of tag's entry in the IFD at ifd
	const auto findEntry = [&](qsizetype ifd, uint16_t tag) -> std::optional<qsizetype> {
		if (ifd < 8 || ifd + 2 > tiff.size())
			return std::nullopt;

		const int entryCount = read16(ifd);
		for (int i = 0; i < entryCount; ++i)
		{
			const qsizetype entry = ifd + 2 + qsizetype{ i } * 12;
			if (entry + 12 > tiff.size())
				break;
			if (read16(entry) == tag)
				return entry;
		}
		return std::nullopt;
	};
	const auto findSubIfdEntry = [&](std::optional<qsizetype> pointerEntry, uint16_t tag) {
		return pointerEntry ? findEntry(read32(*pointerEntry + 8), tag) : std::nullopt;
	};

	ExifMetadata metadata;
	const qsizetype ifd0 = read32(4);
	if (const std::optional<qsizetype> orientation = findEntry(ifd0, OrientationTag))
		metadata.orientation = transformationsForExifOrientation(read16(*orientation + 8));

	const std::optional<qsizetype> exifIfd = findEntry(ifd0, ExifIfdTag);
	if (const std::optional<qsizetype> colorSpace = findSubIfdEntry(exifIfd, ColorSpaceTag))
	{
		const std::optional<qsizetype> interoperabilityIndex = findSubIfdEntry(findSubIfdEntry(exifIfd, InteroperabilityIfdTag), InteroperabilityIndexTag);
		const uint16_t value = read16(*colorSpace + 8);
		metadata.adobeRgb = value == 2 || (value == 0xFFFF && interoperabilityIndex && tiff.sliced(*interoperabilityIndex + 8, 3) == "R03");
	}
	return metadata;
}

} // namespace

ExifMetadata readJpegExif(QByteArrayView jpeg) noexcept
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
			if (const std::optional<ExifMetadata> metadata = readExifApp1(jpeg.sliced(pos + 4, length - 2)))
				return *metadata;
		}

		pos += 2 + length;
	}
	return {};
}

std::optional<ExifMetadata> readExifApp1(QByteArrayView payload) noexcept
{
	if (!payload.startsWith(QByteArrayView{ "Exif\0\0", 6 }))
		return std::nullopt;
	return metadataFromExifTiff(payload.sliced(6));
}

} // namespace ImageDecoding
