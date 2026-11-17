#pragma once

#include "ImageMetadata/Exif.h"

DISABLE_COMPILER_WARNINGS
#include <QByteArray>
#include <QString>
RESTORE_COMPILER_WARNINGS

#include <cstdint>
#include <variant>
#include <vector>

namespace ImageMetadata {

struct ExifRational
{
	int64_t numerator = 0;
	int64_t denominator = 0;

	bool operator==(const ExifRational&) const = default;
};

// An entry's values decoded by its type. Bytes decode to text where the specification or a common writer stores text in them,
// or when they are all printable ASCII; otherwise they stay bytes.
using ExifValue = std::variant<QString, std::vector<int64_t>, std::vector<ExifRational>, std::vector<double>, QByteArray>;

// littleEndian is the byte order of the ExifTags holding entry
[[nodiscard]] ExifValue exifEntryValue(const ExifEntry& entry, bool littleEndian);

} // namespace ImageMetadata
