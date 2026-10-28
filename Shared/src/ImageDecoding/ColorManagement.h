#pragma once

#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QByteArray>
#include <QImage>
RESTORE_COMPILER_WARNINGS

// Color conversion through Little CMS: Qt's own conversion rejects valid profiles, such as a tone curve that dips.
namespace ImageDecoding {

// Adobe RGB (1998), for images that declare it only in EXIF
[[nodiscard]] const QByteArray& adobeRgbIccProfile();

// Converts image from the color profile its pixels are in to sRGB, and tags it sRGB.
// A no-op for an empty or unusable profile, one the pixel format does not match, and one sRGB already reproduces.
// Grayscale stays grayscale; CMYK becomes Format_RGB32.
void convertToSrgb(QImage& image, const QByteArray& iccProfile);

} // namespace ImageDecoding
