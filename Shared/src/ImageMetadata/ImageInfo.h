#pragma once

#include "ImageMetadata/CaptureMetadata.h"
#include "ImageMetadata/Exif.h"

DISABLE_COMPILER_WARNINGS
#include <QByteArray>
#include <QDateTime>
#include <QSize>
#include <QString>
RESTORE_COMPILER_WARNINGS

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace ImageMetadata {

struct ColorProfile
{
	enum class Source : uint8_t
	{
		Embedded,     // the profile of the image's pixels
		ExifAdobeRgb, // none embedded: a JPEG's EXIF declares Adobe RGB, which decoding then applies
		CameraRaw,    // a camera raw's, describing its sensor data or a preview: rendering does not apply it
	};

	Source source;
	std::optional<QString> name; // absent when the profile cannot be read; empty when it has none
};

// What an image file states about itself. Empty or absent where it states nothing.
struct ImageInfo
{
	QString error; // set when the file could not be read; nothing else is then set
	int64_t fileSize = 0;
	QDateTime lastModified;
	QByteArray format; // QImageReader's name for it, e.g. "jpeg", or a camera raw's suffix
	QSize uprightSize;
	std::optional<ColorProfile> colorProfile;
	std::vector<std::pair<QString, QString>> texts; // a PNG's text chunks and a JPEG's comments, by key
	CaptureMetadata capture; // for a camera raw, completed from what LibRaw decodes from the maker notes
	ExifTags otherExif;      // the EXIF entries neither capture nor colorProfile holds
};

// Reads the whole file, except a camera raw's, of which LibRaw reads the headers
[[nodiscard]] ImageInfo readImageInfo(const QString& path);

} // namespace ImageMetadata
