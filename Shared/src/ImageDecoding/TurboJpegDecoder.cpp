#include "ImageDecoding/TurboJpegDecoder.h"

DISABLE_COMPILER_WARNINGS
#include <QObject>
#include <QtEndian>
#include <QtGlobal>
#include <turbojpeg.h>
RESTORE_COMPILER_WARNINGS

#include <cstdint>
#include <memory>

static_assert(Q_BYTE_ORDER == Q_LITTLE_ENDIAN, "TJPF_BGRA lays out Format_RGB32 on little-endian machines only");

namespace ImageDecoding {

namespace {

constexpr char FormatName[] = "jpeg"; // QImageReader's

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

// TransformationNone when the file has no EXIF orientation
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

QString errorString(tjhandle handle)
{
	return QString::fromLocal8Bit(tj3GetErrorStr(handle));
}

} // namespace

std::optional<DecodedImage> decodeJpeg(const QByteArray& encoded)
{
	const std::unique_ptr<void, decltype(&tj3Destroy)> handle{ tj3Init(TJINIT_DECOMPRESS), &tj3Destroy };
	if (!handle)
		return DecodedImage{ .format = FormatName, .error = errorString(nullptr) };

	const auto* data = reinterpret_cast<const unsigned char*>(encoded.constData());
	const auto size = static_cast<size_t>(encoded.size());
	if (tj3DecompressHeader(handle.get(), data, size) != 0)
		return DecodedImage{ .format = FormatName, .error = errorString(handle.get()) };

	const int colorspace = tj3Get(handle.get(), TJPARAM_COLORSPACE);
	if (colorspace == TJCS_CMYK || colorspace == TJCS_YCCK || tj3Get(handle.get(), TJPARAM_PRECISION) != 8)
		return std::nullopt;

	const int width = tj3Get(handle.get(), TJPARAM_JPEGWIDTH);
	const int height = tj3Get(handle.get(), TJPARAM_JPEGHEIGHT);
	const bool grayscale = colorspace == TJCS_GRAY;
	QImage image{ width, height, grayscale ? QImage::Format_Grayscale8 : QImage::Format_RGB32 };
	if (image.isNull())
		return DecodedImage{ .format = FormatName, .error = QObject::tr("Not enough memory to decode a %1x%2 image").arg(width).arg(height) };

	// BGRA, not BGRX: Format_RGB32 requires the fourth byte to be 0xFF, which only BGRA sets
	const int pixelFormat = grayscale ? TJPF_GRAY : TJPF_BGRA;
	// A warning leaves a usable, possibly damaged image: kept, as Qt's plugin keeps it
	if (tj3Decompress8(handle.get(), data, size, image.bits(), static_cast<int>(image.bytesPerLine()), pixelFormat) != 0
		&& tj3GetErrorCode(handle.get()) != TJERR_WARNING)
		return DecodedImage{ .format = FormatName, .error = errorString(handle.get()) };

	return DecodedImage{ .image = std::move(image), .orientation = exifOrientation(encoded), .format = FormatName };
}

} // namespace ImageDecoding
