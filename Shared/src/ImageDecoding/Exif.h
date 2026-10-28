#pragma once

#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QByteArrayView>
#include <QImageIOHandler>
RESTORE_COMPILER_WARNINGS

#include <optional>

namespace ImageDecoding {

// What decoding takes from a JPEG's EXIF
struct ExifMetadata
{
	QImageIOHandler::Transformations orientation = QImageIOHandler::TransformationNone;
	// ColorSpace 2, or uncalibrated with the DCF interoperability index R03: cameras mark Adobe RGB so without embedding a profile
	bool adobeRgb = false;
};

// jpeg starts with SOI. What the file's EXIF does not state keeps its default.
[[nodiscard]] ExifMetadata readJpegExif(QByteArrayView jpeg) noexcept;
// payload is an APP1 segment's, after its length field. Empty unless it holds EXIF.
[[nodiscard]] std::optional<ExifMetadata> readExifApp1(QByteArrayView payload) noexcept;

} // namespace ImageDecoding
