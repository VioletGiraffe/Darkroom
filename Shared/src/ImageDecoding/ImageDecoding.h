#pragma once

#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QByteArray>
#include <QImage>
#include <QImageIOHandler>
#include <QSize>
#include <QString>
#include <QStringList>
RESTORE_COMPILER_WARNINGS

#include <functional>
#include <optional>

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
	// Set for a camera raw's preview when the raw's EXIF declares Adobe RGB: applies when the preview embeds no profile
	bool adobeRgb = false;
};

struct DecodedImage {
	QImage image;                                                                       // as stored; null on failure
	QImageIOHandler::Transformations orientation = QImageIOHandler::TransformationNone; // turns the image upright: see applyOrientation()
	QByteArray iccProfile;                                                              // the color profile of the pixels, for convertToSrgb(); empty when unknown
	QByteArray format;                                                                  // QImageReader's name for it, e.g. "jpeg", or a camera raw's suffix; empty when unrecognized
	QString error;                                                                      // set when image is null
	bool animated = false;                                                              // the file may hold more frames: Qt's reader supports animation for it
};

// Camera raws are read as rawRendering prescribes, other files whole.
[[nodiscard]] EncodedImage readEncodedImage(const QString& path, RawRendering rawRendering);

// Receives a camera raw's embedded JPEG preview, with its format, and the upright size of the LibRaw rendering to come.
// Returning false abandons the read.
using RawPreviewHandler = std::function<bool (EncodedImage preview, QSize uprightRenderSize)>;
// As above, except that a raw rendered by LibRaw (HalfSize, Full) hands onRawPreview its embedded JPEG preview, when it has
// one, before the whole file is read. Empty, without an error, when onRawPreview abandons the read.
[[nodiscard]] EncodedImage readEncodedImage(const QString& path, RawRendering rawRendering, const RawPreviewHandler& onRawPreview);

// Receives the fraction of a LibRaw rendering done, 0 to 1, on the decoding thread. Returning false abandons the rendering.
using RawRenderProgressHandler = std::function<bool (double fraction)>;
// A camera raw read whole reports its rendering's progress to onRawProgress; an abandoned rendering fails.
[[nodiscard]] DecodedImage decode(const EncodedImage& encoded, const RawRenderProgressHandler& onRawProgress = {});
[[nodiscard]] DecodedImage decode(const QByteArray& encoded);
void applyOrientation(QImage& image, QImageIOHandler::Transformations orientation);

// The full decode's size, turned upright, from the headers only.
// Empty unless a DCT-reduced decode of encoded saves time: a JPEG, a camera raw's preview included, that decode() reads
// through libjpeg-turbo, neither progressive nor arithmetic-coded: entropy decoding dominates those at every scale.
[[nodiscard]] QSize readReducibleJpegUprightSize(const EncodedImage& encoded);
// 1 to 8: the smallest DCT scale, in eighths, whose decode is at least as large as uprightSize fitted into uprightBox.
// 8 for an empty uprightBox.
[[nodiscard]] int fittedScaleEighths(QSize uprightSize, QSize uprightBox);
// As decode(), at scaleEighths/8 of each side. Requires a JPEG readReducibleJpegUprightSize() accepts.
[[nodiscard]] DecodedImage decodeReduced(const EncodedImage& encoded, int scaleEighths);

// The image's size once turned upright, read from the file's headers only; empty when they do not state it.
// A camera raw's is the size LibRaw renders it at.
[[nodiscard]] QSize readUprightImageSize(const QString& path);

} // namespace ImageDecoding
