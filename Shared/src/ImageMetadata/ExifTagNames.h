#pragma once

#include "ImageMetadata/Exif.h"

#include <cstdint>
#include <string_view>

namespace ImageMetadata {

// The tag's name in the TIFF, TIFF/EP, EXIF or DNG specification, such as "ExposureTime"; empty for a tag none of them defines
[[nodiscard]] std::string_view exifTagName(ExifDirectory directory, uint16_t tag) noexcept;

} // namespace ImageMetadata
