#pragma once

#include "ImageDecoding/ImageDecoding.h"
#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QByteArray>
#include <QSize>
RESTORE_COMPILER_WARNINGS

#include <optional>

namespace ImageDecoding {

// Empty for the JPEGs left to Qt: CMYK, YCCK, and other than 8 bits per sample.
// A scaleEighths below 8 reduces the decode by libjpeg-turbo's DCT scaling to scaleEighths/8 of each side, rounded up.
[[nodiscard]] std::optional<DecodedImage> decodeJpeg(const QByteArray& encoded, int scaleEighths = 8);
struct JpegHeader {
	QSize storedSize;
	bool progressive = false;
	bool arithmetic = false; // arithmetic entropy coding, not Huffman
};
// Empty for the JPEGs decodeJpeg() leaves to Qt and for an unreadable header.
[[nodiscard]] std::optional<JpegHeader> readJpegHeader(const QByteArray& encoded);

} // namespace ImageDecoding
