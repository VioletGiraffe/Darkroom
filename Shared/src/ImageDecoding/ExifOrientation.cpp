#include "ImageDecoding/ExifOrientation.h"

DISABLE_COMPILER_WARNINGS
#include <QtEndian>
#include <QtGlobal>
RESTORE_COMPILER_WARNINGS

#include <cstdint>

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

// EXIF is a TIFF structure: a byte order mark, 42, then IFD0's offset; IFD0 is an entry count and 12-byte entries.
QImageIOHandler::Transformations orientationFromExifTiff(QByteArrayView tiff) noexcept
{
	if (tiff.size() < 8)
		return QImageIOHandler::TransformationNone;

	const bool littleEndian = tiff.startsWith("II");
	if (!littleEndian && !tiff.startsWith("MM"))
		return QImageIOHandler::TransformationNone;

	const auto read16 = [&](qsizetype offset) {
		return littleEndian ? qFromLittleEndian<uint16_t>(tiff.data() + offset) : qFromBigEndian<uint16_t>(tiff.data() + offset);
	};
	const auto read32 = [&](qsizetype offset) {
		return littleEndian ? qFromLittleEndian<uint32_t>(tiff.data() + offset) : qFromBigEndian<uint32_t>(tiff.data() + offset);
	};

	if (read16(2) != 42)
		return QImageIOHandler::TransformationNone;

	const qsizetype ifd = read32(4);
	if (ifd < 8 || ifd + 2 > tiff.size())
		return QImageIOHandler::TransformationNone;

	const int entryCount = read16(ifd);
	for (int i = 0; i < entryCount; ++i)
	{
		const qsizetype entry = ifd + 2 + qsizetype{ i } * 12;
		if (entry + 12 > tiff.size())
			break;
		// Orientation is a SHORT: its value sits in the first two bytes of the entry's value field
		if (read16(entry) == 0x0112)
			return transformationsForExifOrientation(read16(entry + 8));
	}
	return QImageIOHandler::TransformationNone;
}

} // namespace

QImageIOHandler::Transformations exifOrientation(QByteArrayView jpeg) noexcept
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

		const QByteArrayView payload = jpeg.sliced(pos + 4, length - 2);
		if (marker == 0xE1 && payload.startsWith(QByteArrayView{ "Exif\0\0", 6 })) // APP1
			return orientationFromExifTiff(payload.sliced(6));

		pos += 2 + length;
	}
	return QImageIOHandler::TransformationNone;
}

} // namespace ImageDecoding
