#pragma once

#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QByteArrayView>
#include <QImageIOHandler>
RESTORE_COMPILER_WARNINGS

namespace ImageDecoding {

// jpeg starts with SOI. TransformationNone when the file has no EXIF orientation.
[[nodiscard]] QImageIOHandler::Transformations exifOrientation(QByteArrayView jpeg) noexcept;

} // namespace ImageDecoding
