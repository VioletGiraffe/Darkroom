#include "ImageDecoding/TurboJpegDecoder.h"
#include "ImageDecoding/ColorManagement.h"
#include "ImageDecoding/Exif.h"

DISABLE_COMPILER_WARNINGS
#include <QByteArray>
#include <QObject>
#include <QtGlobal>
#include <turbojpeg.h>
RESTORE_COMPILER_WARNINGS

#include <memory>

static_assert(Q_BYTE_ORDER == Q_LITTLE_ENDIAN, "TJPF_BGRA lays out Format_RGB32 on little-endian machines only");

namespace ImageDecoding {

namespace {

constexpr char FormatName[] = "jpeg"; // QImageReader's

QString errorString(tjhandle handle)
{
	return QString::fromLocal8Bit(tj3GetErrorStr(handle));
}

// Empty when the file embeds no ICC profile. Requires the header read with TJPARAM_SAVEMARKERS 2 or 4.
QByteArray embeddedIccProfile(tjhandle handle)
{
	unsigned char* icc = nullptr;
	size_t iccSize = 0;
	if (tj3GetICCProfile(handle, &icc, &iccSize) != 0 || !icc)
		return {};

	const std::unique_ptr<unsigned char, decltype(&tj3Free)> iccOwner{ icc, &tj3Free };
	return QByteArray{ reinterpret_cast<const char*>(icc), static_cast<qsizetype>(iccSize) };
}

} // namespace

std::optional<DecodedImage> decodeJpeg(const QByteArray& encoded)
{
	const std::unique_ptr<void, decltype(&tj3Destroy)> handle{ tj3Init(TJINIT_DECOMPRESS), &tj3Destroy };
	if (!handle)
		return DecodedImage{ .format = FormatName, .error = errorString(nullptr) };

	const auto* data = reinterpret_cast<const unsigned char*>(encoded.constData());
	const auto size = static_cast<size_t>(encoded.size());
	tj3Set(handle.get(), TJPARAM_SAVEMARKERS, 4); // the ICC profile only
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
	QByteArray iccProfile = embeddedIccProfile(handle.get());

	// BGRA, not BGRX: Format_RGB32 requires the fourth byte to be 0xFF, which only BGRA sets
	const int pixelFormat = grayscale ? TJPF_GRAY : TJPF_BGRA;
	// A warning leaves a usable, possibly damaged image: kept, as Qt's plugin keeps it
	if (tj3Decompress8(handle.get(), data, size, image.bits(), static_cast<int>(image.bytesPerLine()), pixelFormat) != 0
		&& tj3GetErrorCode(handle.get()) != TJERR_WARNING)
		return DecodedImage{ .format = FormatName, .error = errorString(handle.get()) };

	const ExifMetadata exif = readJpegExif(encoded);
	if (iccProfile.isEmpty() && exif.adobeRgb)
		iccProfile = adobeRgbIccProfile();
	return DecodedImage{ .image = std::move(image), .orientation = exif.orientation, .iccProfile = std::move(iccProfile), .format = FormatName };
}

} // namespace ImageDecoding
