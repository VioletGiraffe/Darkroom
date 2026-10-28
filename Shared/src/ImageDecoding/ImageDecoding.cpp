#include "ImageDecoding/ImageDecoding.h"
#include "ImageDecoding/ColorManagement.h"
#include "ImageDecoding/Exif.h"
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
#include <QSize>
#include <QTransform>
#include <QtEndian>
RESTORE_COMPILER_WARNINGS

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
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

// The file is at SOI. Empty unless an SOF segment precedes the image data.
static QSize readJpegUprightSize(thin_io::buffered_reader& file)
{
	// EXIF, in APP1, precedes SOF
	QImageIOHandler::Transformations orientation = QImageIOHandler::TransformationNone;
	// After SOI, each marker is FF, its code and a big-endian length that counts itself; the image data starts at SOS
	for (uint64_t pos = 2;;)
	{
		file.set_pos(pos);
		const std::optional<std::span<const std::byte>> header = file.peek(4);
		if (!header || header->size() < 4 || (*header)[0] != std::byte{ 0xFF })
			return {};

		const auto marker = static_cast<uint8_t>((*header)[1]);
		if (marker == 0xFF) // fill byte
		{
			++pos;
			continue;
		}
		if (marker == 0xDA || marker == 0xD9) // SOS, EOI
			return {};

		const uint16_t length = qFromBigEndian<uint16_t>(header->data() + 2);
		if (length < 2)
			return {};

		file.set_pos(pos + 4);
		if (marker == 0xE1) // APP1
		{
			const std::optional<std::span<const std::byte>> payload = file.peek(length - 2u);
			if (!payload)
				return {};
			if (const std::optional<ExifMetadata> exif = readExifApp1(QByteArrayView{ payload->data(), static_cast<qsizetype>(payload->size()) }))
				orientation = exif->orientation;
		}
		// SOF0 to SOF15, except DHT, JPG and DAC: precision, height, width
		else if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC)
		{
			const std::optional<std::span<const std::byte>> frame = file.peek(5);
			if (!frame || frame->size() < 5)
				return {};
			const QSize stored{ qFromBigEndian<uint16_t>(frame->data() + 3), qFromBigEndian<uint16_t>(frame->data() + 1) };
			return orientation.testFlag(QImageIOHandler::TransformationRotate90) ? stored.transposed() : stored;
		}

		pos += 2u + length;
	}
}

static QSize readUprightSizeWithQt(const QString& path)
{
	QImageReader reader{ path };
	reader.setDecideFormatFromContent(true); // as decodeWithQt, which gives no format hint
	const QSize stored = reader.size();
	return reader.transformation().testFlag(QImageIOHandler::TransformationRotate90) ? stored.transposed() : stored;
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

QSize readUprightImageSize(const QString& path)
{
	thin_io::buffered_reader file;
	if (!file.open(thin_io::native_path(path).data()))
		return {};
	if (!rawFileSuffix(path).isEmpty())
		return readRawUprightSize(file);

	const std::optional<std::span<const std::byte>> start = file.peek(3);
	if (start && isJpeg(QByteArrayView{ start->data(), static_cast<qsizetype>(start->size()) }))
		return readJpegUprightSize(file);
	return readUprightSizeWithQt(path);
}

} // namespace ImageDecoding
