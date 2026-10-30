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
[[nodiscard]] DecodedImage decodeRaw(const QByteArray& file, RawRendering rawRendering, const RawRenderProgressHandler& onProgress);
// From the raw's metadata, without unpacking; empty when LibRaw cannot parse it
[[nodiscard]] QSize readRawUprightSize(thin_io::buffered_reader& file);

} // namespace ImageDecoding
