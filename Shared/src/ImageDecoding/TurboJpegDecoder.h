#pragma once

#include "ImageDecoding/ImageDecoding.h"

#include <optional>

namespace ImageDecoding {

// Empty for the JPEGs left to Qt: CMYK, YCCK, and other than 8 bits per sample.
[[nodiscard]] std::optional<DecodedImage> decodeJpeg(const QByteArray& encoded);

} // namespace ImageDecoding
