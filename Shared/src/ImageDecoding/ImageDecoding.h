#pragma once

#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QByteArray>
#include <QByteArrayView>
#include <QImage>
#include <QImageIOHandler>
#include <QIODevice>
#include <QString>
RESTORE_COMPILER_WARNINGS

// Image loading routed by content: JPEG through libjpeg-turbo, every other format through Qt's image plugins.
// Split in two for the pipelines that read on the I/O pool and decode on the CPU pool: readEncodedImage, then decode.
namespace ImageDecoding {

enum class Intent { Thumbnail, Full };
enum class Decoder { Qt, TurboJpeg };

// Everything decode() needs: decoding never touches the file
struct EncodedImage {
	QByteArray bytes;
	QString error; // set when the read failed
};

struct DecodedImage {
	QImage image;                                                                       // as stored; null on failure
	QImageIOHandler::Transformations orientation = QImageIOHandler::TransformationNone; // turns the image upright: see applyOrientation()
	QByteArray format;                                                                  // QImageReader's name for it, e.g. "jpeg"; empty when unrecognized
	QString error;                                                                      // set when image is null
};

// The leading bytes decoderFor() inspects
constexpr qsizetype SignatureSize = 3;

// Reads from the device's current position: a caller that sniffed it with peek() pays for no second open.
// Every format supported so far is read whole for either intent.
[[nodiscard]] EncodedImage readEncodedImage(QIODevice& device, Intent intent);
[[nodiscard]] EncodedImage readEncodedImage(const QString& path, Intent intent);
[[nodiscard]] Decoder decoderFor(QByteArrayView encoded) noexcept;
[[nodiscard]] DecodedImage decode(const EncodedImage& encoded);
[[nodiscard]] DecodedImage decode(const QByteArray& encoded);
void applyOrientation(QImage& image, QImageIOHandler::Transformations orientation);

} // namespace ImageDecoding
