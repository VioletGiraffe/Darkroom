#pragma once

#include "ImageMetadata/ExifValues.h"
#include "ImageMetadata/ImageInfo.h"

DISABLE_COMPILER_WARNINGS
#include <QString>
RESTORE_COMPILER_WARNINGS

#include <vector>

// ImageInfoPanel's content: an ImageInfo's values as text, in groups
namespace ImageInfoRows {

struct Row
{
	QString key;
	QString value;
	QString keyToolTip; // empty when the key needs none
};

struct Group
{
	QString title;
	std::vector<Row> rows;
};

// Groups without rows are left out
[[nodiscard]] std::vector<Group> groups(const QString& path, const ImageMetadata::ImageInfo& info);

// The value formats, public for the tests
[[nodiscard]] QString exposureTimeText(ImageMetadata::ExifRational seconds);
[[nodiscard]] QString fNumberText(double fNumber);
[[nodiscard]] QString exposureBiasText(ImageMetadata::ExifRational ev);
[[nodiscard]] QString exifValueText(const ImageMetadata::ExifValue& value);

} // namespace ImageInfoRows
