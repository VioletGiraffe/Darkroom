#pragma once

#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QByteArray>
#include <QByteArrayView>
#include <QString>
#include <QStringList>
RESTORE_COMPILER_WARNINGS

#include <functional>

class QImage;
class QMimeType;

// Saving images: JPEG through libjpeg-turbo, the other formats through Qt's writer plugins
namespace ImageEncoding {

constexpr int DefaultQuality = 90;

enum class JpegSubsampling { Chroma420, Chroma444 };

struct EncodingOptions
{
	QString mimeType; // a writable type
	int quality = DefaultQuality; // 1 to 100, for lossy types only: the PNG writer maps quality to its compression level
	JpegSubsampling jpegSubsampling = JpegSubsampling::Chroma420;
};

struct EncodedFile
{
	QByteArray bytes; // empty on failure
	QString error;
};

// The formats images can be saved in, as MIME types: a fixed list, filtered to those a writer is present for
[[nodiscard]] QStringList writableMimeTypes();
// The writable types to offer the user: without HEIF when HEIC is there, which writes the same files under the conventional extension
[[nodiscard]] QStringList offeredMimeTypes();
// The entry of writableMimeTypes() that is type; empty when there is none. Compares resolved types: a platform's MIME database may
// know a listed name as an alias.
[[nodiscard]] QString writableMimeTypeOf(const QMimeType& type);
// Requires a writable type
[[nodiscard]] bool isLossy(const QString& mimeType);
// True for the types encodeImage() carries EXIF into
[[nodiscard]] bool takesExif(const QString& mimeType);

// image encoded as options say; iccProfile is the color profile of its pixels, empty when unknown.
// - A JPEG has optimized Huffman tables and embeds iccProfile. An image with alpha is composed onto white: JPEG has no alpha.
// - The other formats embed the profile only when QColorSpace can parse it.
// - A type taking EXIF carries sourceFile's, fitted to image: see ImageMetadata::exifApp1ForOutput(). sourceFile may be empty.
[[nodiscard]] EncodedFile encodeImage(const QImage& image, const QByteArray& iccProfile, const EncodingOptions& options, QByteArrayView sourceFile);

// Empty on success, otherwise the error. A failed write leaves path as it was.
// beforeReplacing, when given, runs once bytes are written, just before they take path's place.
[[nodiscard]] QString writeFileAtomically(const QString& path, QByteArrayView bytes, const std::function<void()>& beforeReplacing = {});

// encodeImage(), then writeFileAtomically()
[[nodiscard]] QString writeImage(const QImage& image, const QByteArray& iccProfile, const QString& targetPath, const EncodingOptions& options,
	QByteArrayView sourceFile);

} // namespace ImageEncoding
