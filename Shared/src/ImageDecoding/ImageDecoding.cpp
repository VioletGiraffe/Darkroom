#include "ImageDecoding/ImageDecoding.h"
#include "ImageDecoding/ColorManagement.h"
#include "ImageDecoding/LibRawDecoder.h"
#include "ImageDecoding/TurboJpegDecoder.h"
#include "buffered_reader.hpp" // thin_io
#include "qt_paths.hpp"        // thin_io

DISABLE_COMPILER_WARNINGS
#include <QBuffer>
#include <QByteArrayView>
#include <QColorSpace>
#include <QFileInfo>
#include <QImageReader>
#include <QTransform>
RESTORE_COMPILER_WARNINGS

#include <cstdint>
#include <optional>
#include <utility>

namespace ImageDecoding {

static bool isJpeg(QByteArrayView encoded) noexcept
{
	return encoded.startsWith("\xFF\xD8\xFF"); // SOI, then the next marker
}

// Lowercase; empty unless the path has a camera raw's extension
static QString rawFileSuffix(const QString& path)
{
	QString suffix = QFileInfo{ path }.suffix().toLower();
	return rawFileSuffixes().contains(suffix) ? suffix : QString{};
}

static EncodedImage readWholeFile(thin_io::buffered_reader& file)
{
	const std::optional<uint64_t> size = file.size();
	if (!size)
		return { .error = QString::fromStdString(thin_io::buffered_reader::text_for_last_error()) };

	QByteArray bytes{ static_cast<qsizetype>(*size), Qt::Uninitialized };
	file.set_pos(0);
	const std::optional<uint64_t> bytesRead = file.read(bytes.data(), *size);
	if (!bytesRead)
		return { .error = QString::fromStdString(thin_io::buffered_reader::text_for_last_error()) };

	bytes.truncate(static_cast<qsizetype>(*bytesRead)); // the file shrank
	return { .bytes = std::move(bytes) };
}

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
	{
		decoded.orientation = reader.transformation();
		// The profile as Qt parsed it from the file, or one Qt describes the file's color chunks with, such as PNG's gAMA
		decoded.iccProfile = decoded.image.colorSpace().iccProfile();
		decoded.animated = reader.supportsAnimation();
	}
	return decoded;
}

EncodedImage readEncodedImage(const QString& path, RawRendering rawRendering)
{
	thin_io::buffered_reader file;
	if (!file.open(thin_io::native_path(path).data()))
		return { .error = QString::fromStdString(thin_io::buffered_reader::text_for_last_error()) };

	const QString rawSuffix = rawFileSuffix(path);
	if (rawSuffix.isEmpty())
		return readWholeFile(file);

	EncodedImage encoded;
	if (std::optional<EncodedImage> preview = rawRendering == RawRendering::EmbeddedPreview ? readRawPreview(file) : std::nullopt)
		encoded = std::move(*preview);
	else
	{
		encoded = readWholeFile(file);
		encoded.rawRendering = rawRendering == RawRendering::EmbeddedPreview ? RawRendering::HalfSize : rawRendering;
	}
	encoded.format = rawSuffix.toLatin1();
	return encoded;
}

DecodedImage decode(const EncodedImage& encoded)
{
	DecodedImage decoded = !encoded.error.isEmpty() ? DecodedImage{ .error = encoded.error }
		: encoded.rawRendering                     ? decodeRaw(encoded.bytes, *encoded.rawRendering)
		                                           : decode(encoded.bytes);
	if (!encoded.format.isEmpty())
		decoded.format = encoded.format;
	if (encoded.orientation)
		decoded.orientation = *encoded.orientation;
	if (encoded.adobeRgb && decoded.iccProfile.isEmpty() && !decoded.image.isNull())
		decoded.iccProfile = adobeRgbIccProfile();
	return decoded;
}

DecodedImage decode(const QByteArray& encoded)
{
	if (isJpeg(encoded))
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
