#pragma once

#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QByteArray>
#include <QByteArrayView>
#include <QImageIOHandler>
#include <QSize>
RESTORE_COMPILER_WARNINGS

#include <cstdint>
#include <optional>
#include <vector>

namespace ImageMetadata {

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
// The payload of jpeg's first APP1 segment that holds EXIF, as readExifApp1() takes it; empty when it has none. jpeg starts with SOI.
[[nodiscard]] std::optional<QByteArrayView> findExifApp1(QByteArrayView jpeg) noexcept;

enum class ExifDirectory : uint8_t { Ifd0, Exif, Gps, Interoperability, Ifd1 };

struct ExifEntry
{
	ExifDirectory directory;
	uint16_t tag;
	uint16_t type;    // TIFF's field type, 1 (BYTE) to 13 (IFD)
	uint32_t count;   // the number of values of type
	QByteArray value; // count values of type, in the byte order of the ExifTags holding the entry

	bool operator==(const ExifEntry&) const = default;
};

// The pointers to sub-IFDs are not entries: exifApp1FromTags() writes its own
struct ExifTags
{
	bool littleEndian = true;
	std::vector<ExifEntry> entries;

	bool operator==(const ExifTags&) const = default;
};

// payload as readExifApp1() takes it; no entries unless it holds EXIF. Leaves out entries with a value outside the payload.
[[nodiscard]] ExifTags exifTagsFromApp1(QByteArrayView payload);
// The EXIF directories LibRaw parses in a camera raw; no entries when LibRaw cannot open it. A tag found twice keeps its first value.
[[nodiscard]] ExifTags exifTagsFromRaw(QByteArrayView rawFile);

// Fits tags to an image written upright at uprightOutputSize:
// - leaves out IFD1, the MakerNote, and IFD0's tags other than the descriptive ones: those refer to the source file's layout;
// - sets Orientation to 1, and PixelXDimension and PixelYDimension to the output size, adding them when the Exif IFD has other entries.
void prepareExifTagsForOutput(ExifTags& tags, QSize uprightOutputSize);
// An APP1 payload holding the entries of tags, except IFD1's. Empty without entries outside IFD1, or when they do not fit a segment.
[[nodiscard]] QByteArray exifApp1FromTags(const ExifTags& tags);

// The APP1 payload for a JPEG of sourceFile's image written upright at uprightOutputSize; empty when sourceFile has no EXIF.
// - A JPEG's own payload is copied, its orientation and pixel dimensions set in place, and its thumbnail unlinked when the source was
//   not upright: in place, the MakerNote's offsets stay valid.
// - Any other file goes through exifTagsFromRaw() and prepareExifTagsForOutput().
[[nodiscard]] QByteArray exifApp1ForOutput(QByteArrayView sourceFile, QSize uprightOutputSize);
// jpeg with an APP1 segment holding exifApp1 inserted after SOI, and after APP0 when present; jpeg itself when exifApp1 is empty.
// jpeg starts with SOI and holds no EXIF.
[[nodiscard]] QByteArray jpegWithExifApp1(QByteArrayView jpeg, QByteArrayView exifApp1);

} // namespace ImageMetadata
