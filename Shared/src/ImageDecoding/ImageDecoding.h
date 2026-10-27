#pragma once

#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QByteArray>
#include <QByteArrayView>
#include <QImage>
#include <QImageIOHandler>
#include <QString>
#include <QStringList>
RESTORE_COMPILER_WARNINGS

#include <optional>

namespace thin_io { class buffered_reader; }

// Image loading: camera raws through LibRaw, chosen by extension; other files by content, JPEG through libjpeg-turbo and
// every other format through Qt's image plugins.
// Split in two for the pipelines that read on the I/O pool and decode on the CPU pool: readEncodedImage, then decode.
namespace ImageDecoding {

// How a camera raw becomes an image; other files ignore it
enum class RawRendering {
	EmbeddedPreview, // the largest embedded JPEG; HalfSize for a raw without one
	HalfSize,        // LibRaw's rendering without demosaicing: each 2x2 sensor block is one pixel
	Full,            // LibRaw's demosaiced rendering
};
enum class Decoder { Qt, TurboJpeg, LibRaw };

// Lowercase extensions of the camera raw formats. Inline: callers that only list files link no decoder.
[[nodiscard]] inline const QStringList& rawFileSuffixes()
{
	static const QStringList suffixes{
		"3fr", "ari", "arw", "bay", "cr2", "cr3", "crw", "dcr", "dng", "erf", "fff", "iiq", "k25", "kdc", "mdc", "mef",
		"mos", "mrw", "nef", "nefx", "nrw", "orf", "pef", "raf", "raw", "rw2", "rwl", "sr2", "srf", "srw"
	};
	return suffixes;
}

// Everything decode() needs: decoding never touches the file
struct EncodedImage {
	QByteArray bytes;
	QString error; // set when the read failed
	// Set for camera raws, the orientation only with the embedded preview: they replace what decode() finds in the bytes
	QByteArray format;
	std::optional<QImageIOHandler::Transformations> orientation;
	// Set for a camera raw read whole, never to EmbeddedPreview: decode() renders it through LibRaw
	std::optional<RawRendering> rawRendering;
};

struct DecodedImage {
	QImage image;                                                                       // as stored; null on failure
	QImageIOHandler::Transformations orientation = QImageIOHandler::TransformationNone; // turns the image upright: see applyOrientation()
	QByteArray format;                                                                  // QImageReader's name for it, e.g. "jpeg", or a camera raw's suffix; empty when unrecognized
	QString error;                                                                      // set when image is null
};

// The leading bytes decoderFor() inspects
constexpr qsizetype SignatureSize = 3;

// Reads from the start of the file, whatever its position: a caller that sniffed it with peek() pays for no second open.
// Camera raws are read as rawRendering prescribes, other files whole.
[[nodiscard]] EncodedImage readEncodedImage(thin_io::buffered_reader& file, const QString& path, RawRendering rawRendering);
[[nodiscard]] EncodedImage readEncodedImage(const QString& path, RawRendering rawRendering);
[[nodiscard]] Decoder decoderFor(const QString& path, QByteArrayView signature);
[[nodiscard]] DecodedImage decode(const EncodedImage& encoded);
[[nodiscard]] DecodedImage decode(const QByteArray& encoded);
void applyOrientation(QImage& image, QImageIOHandler::Transformations orientation);

} // namespace ImageDecoding
