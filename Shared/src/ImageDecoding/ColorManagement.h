#pragma once

#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QByteArray>
#include <QImage>
#include <QString>
RESTORE_COMPILER_WARNINGS

#include <optional>

// Color profiles through Little CMS: Qt rejects valid ones, such as a tone curve that dips.
namespace ImageDecoding {

// Adobe RGB (1998), for images that declare it only in EXIF
[[nodiscard]] const QByteArray& adobeRgbIccProfile();

// The profile's description, its name; empty when it has none, absent when the profile cannot be read
[[nodiscard]] std::optional<QString> iccProfileDescription(const QByteArray& iccProfile);

// Converts image from the color profile its pixels are in to sRGB, and tags it sRGB.
// A no-op for an empty or unusable profile, one the pixel format does not match, and one sRGB already reproduces.
// Grayscale stays grayscale; CMYK becomes Format_RGB32.
void convertToSrgb(QImage& image, const QByteArray& iccProfile);

} // namespace ImageDecoding
