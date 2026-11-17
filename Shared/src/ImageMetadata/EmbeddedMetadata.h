#pragma once

#include "ImageMetadata/Exif.h"

DISABLE_COMPILER_WARNINGS
#include <QByteArray>
#include <QByteArrayView>
RESTORE_COMPILER_WARNINGS

namespace ImageMetadata {

// The EXIF and the ICC profile an image file embeds
struct EmbeddedMetadata
{
	ExifTags exif;
	QByteArray iccProfile; // empty without one
};

// A JPEG's, a TIFF's, a PNG's or a WebP's, by content; empty for any other file
[[nodiscard]] EmbeddedMetadata readEmbeddedMetadata(QByteArrayView file);

} // namespace ImageMetadata
