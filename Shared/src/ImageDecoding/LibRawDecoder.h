#pragma once

#include "ImageDecoding/ImageDecoding.h"

namespace ImageDecoding {

// The largest JPEG preview a camera raw embeds, oriented as the raw is; format is left to the caller
[[nodiscard]] EncodedImage readRawPreview(thin_io::buffered_reader& file);

} // namespace ImageDecoding
