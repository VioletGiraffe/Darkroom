#include "ImageDecoding/ImageDecoding.h"
#include "ImageDecoding/TurboJpegDecoder.h"

DISABLE_COMPILER_WARNINGS
#include <QBuffer>
#include <QFile>
#include <QImageReader>
#include <QTransform>
RESTORE_COMPILER_WARNINGS

#include <optional>
#include <utility>

namespace ImageDecoding {

static DecodedImage decodeWithQt(const QByteArray& encoded)
{
	QBuffer buffer;
	buffer.setData(encoded);
	buffer.open(QIODevice::ReadOnly);
	QImageReader reader(&buffer); // A format hint would suppress content detection.

	DecodedImage decoded{ .format = reader.format() };
	decoded.image = reader.read();
	if (decoded.image.isNull())
		decoded.error = reader.errorString();
	else
		decoded.orientation = reader.transformation();
	return decoded;
}

EncodedImage readEncodedImage(QIODevice& device, [[maybe_unused]] Intent intent)
{
	return { .bytes = device.readAll() };
}

EncodedImage readEncodedImage(const QString& path, Intent intent)
{
	QFile file{ path };
	if (!file.open(QIODevice::ReadOnly))
		return { .error = file.errorString() };
	return readEncodedImage(file, intent);
}

Decoder decoderFor(QByteArrayView encoded) noexcept
{
	// JPEG: SOI, then the next marker
	if (encoded.startsWith("\xFF\xD8\xFF"))
		return Decoder::TurboJpeg;
	return Decoder::Qt;
}

DecodedImage decode(const EncodedImage& encoded)
{
	if (!encoded.error.isEmpty())
		return { .error = encoded.error };
	return decode(encoded.bytes);
}

DecodedImage decode(const QByteArray& encoded)
{
	if (decoderFor(encoded) == Decoder::TurboJpeg)
	{
		if (std::optional<DecodedImage> decoded = decodeJpeg(encoded))
			return std::move(*decoded);
	}
	return decodeWithQt(encoded);
}

void applyOrientation(QImage& image, QImageIOHandler::Transformations orientation)
{
	// The flags compose as flips first, then the 90-degree turn
	Qt::Orientations flips;
	if (orientation.testFlag(QImageIOHandler::TransformationMirror))
		flips |= Qt::Horizontal;
	if (orientation.testFlag(QImageIOHandler::TransformationFlip))
		flips |= Qt::Vertical;
	image.flip(flips);
	if (orientation.testFlag(QImageIOHandler::TransformationRotate90))
		image = image.transformed(QTransform{}.rotate(90));
}

} // namespace ImageDecoding
