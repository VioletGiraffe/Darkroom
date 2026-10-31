#include "ImageDecoding/TurboJpegDecoder.h"
#include "ImageDecoding/ColorManagement.h"
#include "ImageDecoding/Exif.h"
#include "assert/advanced_assert.h"

DISABLE_COMPILER_WARNINGS
#include <QByteArray>
#include <QObject>
#include <QtGlobal>
#include <turbojpeg.h>
RESTORE_COMPILER_WARNINGS

#include <memory>
#include <span>

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

// Requires the header read
bool isLeftToQt(tjhandle handle)
{
	const int colorspace = tj3Get(handle, TJPARAM_COLORSPACE);
	return colorspace == TJCS_CMYK || colorspace == TJCS_YCCK || tj3Get(handle, TJPARAM_PRECISION) != 8;
}

// libjpeg-turbo's factor equal to scaleEighths/8; absent when it offers none
std::optional<tjscalingfactor> scalingFactorForEighths(int scaleEighths)
{
	int count = 0;
	const tjscalingfactor* factors = tj3GetScalingFactors(&count);
	for (const tjscalingfactor& factor : std::span{ factors, factors ? static_cast<size_t>(count) : 0u })
	{
		if (factor.num * 8 == scaleEighths * factor.denom)
			return factor;
	}
	return std::nullopt;
}

} // namespace

std::optional<DecodedImage> decodeJpeg(const QByteArray& encoded, int scaleEighths)
{
	const std::unique_ptr<void, decltype(&tj3Destroy)> handle{ tj3Init(TJINIT_DECOMPRESS), &tj3Destroy };
	if (!handle)
		return DecodedImage{ .format = FormatName, .error = errorString(nullptr) };

	const auto* data = reinterpret_cast<const unsigned char*>(encoded.constData());
	const auto size = static_cast<size_t>(encoded.size());
	tj3Set(handle.get(), TJPARAM_SAVEMARKERS, 4); // the ICC profile only
	if (tj3DecompressHeader(handle.get(), data, size) != 0)
		return DecodedImage{ .format = FormatName, .error = errorString(handle.get()) };

	if (isLeftToQt(handle.get()))
		return std::nullopt;

	const std::optional<tjscalingfactor> factor = scalingFactorForEighths(scaleEighths);
	assert_and_return_r(factor, (DecodedImage{ .format = FormatName, .error = QObject::tr("Unsupported JPEG scale: %1/8").arg(scaleEighths) }));
	const tjscalingfactor scale = *factor;
	if (tj3SetScalingFactor(handle.get(), scale) != 0)
		return DecodedImage{ .format = FormatName, .error = errorString(handle.get()) };

	const int width = TJSCALED(tj3Get(handle.get(), TJPARAM_JPEGWIDTH), scale);
	const int height = TJSCALED(tj3Get(handle.get(), TJPARAM_JPEGHEIGHT), scale);
	const bool grayscale = tj3Get(handle.get(), TJPARAM_COLORSPACE) == TJCS_GRAY;
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

std::optional<JpegHeader> readJpegHeader(const QByteArray& encoded)
{
	const std::unique_ptr<void, decltype(&tj3Destroy)> handle{ tj3Init(TJINIT_DECOMPRESS), &tj3Destroy };
	if (!handle || tj3DecompressHeader(handle.get(), reinterpret_cast<const unsigned char*>(encoded.constData()), static_cast<size_t>(encoded.size())) != 0
		|| isLeftToQt(handle.get()))
		return std::nullopt;

	return JpegHeader{
		.storedSize = QSize{ tj3Get(handle.get(), TJPARAM_JPEGWIDTH), tj3Get(handle.get(), TJPARAM_JPEGHEIGHT) },
		.progressive = tj3Get(handle.get(), TJPARAM_PROGRESSIVE) == 1,
		.arithmetic = tj3Get(handle.get(), TJPARAM_ARITHMETIC) == 1,
	};
}

} // namespace ImageDecoding
