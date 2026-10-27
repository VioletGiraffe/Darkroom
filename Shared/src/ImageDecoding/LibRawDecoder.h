#pragma once

#include "ImageDecoding/ImageDecoding.h"

#include <optional>

namespace ImageDecoding {

// The largest JPEG preview a camera raw embeds, oriented as the raw is; format is left to the caller.
// Empty when the raw has no JPEG preview.
[[nodiscard]] std::optional<EncodedImage> readRawPreview(thin_io::buffered_reader& file);
// A whole camera raw file rendered by LibRaw, rawRendering being HalfSize or Full; format is left to the caller
[[nodiscard]] DecodedImage decodeRaw(const QByteArray& file, RawRendering rawRendering);

} // namespace ImageDecoding
