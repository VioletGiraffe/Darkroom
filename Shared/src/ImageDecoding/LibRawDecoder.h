#pragma once

#include "ImageDecoding/ImageDecoding.h"

DISABLE_COMPILER_WARNINGS
#include <QSize>
RESTORE_COMPILER_WARNINGS

#include <optional>

namespace thin_io { class buffered_reader; }

namespace ImageDecoding {

// The largest JPEG preview a camera raw embeds, oriented as the raw is; format is left to the caller.
// Empty when the raw has no JPEG preview. uprightRenderSize, when given, receives the upright size decodeRaw() renders the raw
// at with rawRendering, which must then be HalfSize or Full.
[[nodiscard]] std::optional<EncodedImage> readRawPreview(thin_io::buffered_reader& file, RawRendering rawRendering, QSize* uprightRenderSize);
// A whole camera raw file rendered by LibRaw, rawRendering being HalfSize or Full; format is left to the caller. onProgress may be empty.
// The rendering's exposure is matched to the raw's embedded JPEG preview: from decodedPreview when given, else decoded from file.
[[nodiscard]] DecodedImage decodeRaw(const QByteArray& file, RawRendering rawRendering, const RawRenderProgressHandler& onProgress,
	const DecodedRawPreviewSource& decodedPreview);

// decodeRaw()'s rendering before its tone curve, for fitting that curve: see the [tonecurvedata] test.
// decodeRaw() applies cameraExposureEv + previewShiftEv before the curve.
struct LinearRawRendering {
	QImage image;                  // linear 16-bit in sRGB primaries, as stored: Format_RGBX64, or Format_Grayscale16 for a monochrome raw; null on failure
	double cameraExposureEv = 0.0; // from the raw's metadata: the exposure the camera's own rendering adds
	double previewShiftEv = 0.0;   // matches the curve's midtones to the embedded JPEG preview's; 0 without a usable preview
	QString error;
};
[[nodiscard]] LinearRawRendering decodeRawLinear(const QByteArray& file, RawRendering rawRendering);

// From the raw's metadata, without unpacking; empty when LibRaw cannot parse it
[[nodiscard]] QSize readRawUprightSize(thin_io::buffered_reader& file);

} // namespace ImageDecoding
