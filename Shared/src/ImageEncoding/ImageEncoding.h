#pragma once

#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QByteArrayView>
#include <QString>
#include <QStringList>
RESTORE_COMPILER_WARNINGS

class QImage;
class QMimeType;

// Saving images through Qt's writer plugins
namespace ImageEncoding {

constexpr int DefaultQuality = 90;

// The formats images can be saved in, as MIME types: a fixed list, filtered to those a writer plugin is present for
[[nodiscard]] QStringList writableMimeTypes();
// The writable types to offer the user: without HEIF when HEIC is there, which writes the same files under the conventional extension
[[nodiscard]] QStringList offeredMimeTypes();
// The entry of writableMimeTypes() that is type; empty when there is none. Compares resolved types: a platform's MIME database may
// know a listed name as an alias.
[[nodiscard]] QString writableMimeTypeOf(const QMimeType& type);
// Requires a writable type
[[nodiscard]] bool isLossy(const QString& mimeType);
// True for the types writeImage() carries EXIF into
[[nodiscard]] bool takesExif(const QString& mimeType);

// Writes image to targetPath as mimeType, a writable type; empty on success, otherwise the error. A failed write leaves targetPath as it was.
// - quality, 0 to 100, applies to lossy types only: the PNG writer maps quality to its compression level.
// - A type taking EXIF carries sourceFile's, fitted to image: see ImageMetadata::exifApp1ForOutput(). sourceFile may be empty.
// - A JPEG of an image with alpha is composed onto white: JPEG has no alpha.
[[nodiscard]] QString writeImage(const QImage& image, const QString& targetPath, const QString& mimeType, int quality, QByteArrayView sourceFile);

} // namespace ImageEncoding
