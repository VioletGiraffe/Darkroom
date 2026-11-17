#pragma once

#include "ImageMetadata/Exif.h"
#include "ImageMetadata/ExifValues.h"

DISABLE_COMPILER_WARNINGS
#include <QDate>
#include <QString>
#include <QTime>
RESTORE_COMPILER_WARNINGS

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ImageMetadata {

// The camera's clock when the photo was taken
struct CaptureTime
{
	QDate date;
	QTime time;
	std::optional<int> utcOffsetSeconds;

	bool operator==(const CaptureTime&) const = default;
};

struct GpsPosition
{
	double latitude = 0.0;  // degrees, north positive
	double longitude = 0.0; // degrees, east positive
	std::optional<double> altitude; // metres above sea level

	bool operator==(const GpsPosition&) const = default;
};

// The commonly wanted EXIF fields, decoded. Each is empty or absent when the EXIF does not state it.
struct CaptureMetadata
{
	QString make, model;
	QString lensMake, lens, lensSerial, bodySerial;
	QString software, artist, copyright, description;
	std::optional<CaptureTime> captureTime;
	std::optional<ExifRational> exposureTime; // seconds
	std::optional<double> fNumber;
	std::optional<uint32_t> iso;
	std::optional<ExifRational> exposureBias; // EV
	std::optional<double> focalLength;        // mm
	std::optional<uint32_t> focalLength35mm;  // mm
	std::optional<GpsPosition> gps;
	// EXIF codes; see the name functions below
	std::optional<uint16_t> orientation, exposureProgram, meteringMode, flash, whiteBalance, colorSpace;

	bool operator==(const CaptureMetadata&) const = default;
};

// Decodes the entries CaptureMetadata holds and removes them from tags; entries that fail to decode stay
[[nodiscard]] CaptureMetadata takeCaptureMetadata(ExifTags& tags);

// English names of EXIF codes; empty for a code the specification does not define
[[nodiscard]] std::string_view orientationName(uint16_t orientation) noexcept;
[[nodiscard]] std::string_view exposureProgramName(uint16_t exposureProgram) noexcept;
[[nodiscard]] std::string_view meteringModeName(uint16_t meteringMode) noexcept;
[[nodiscard]] std::string_view whiteBalanceName(uint16_t whiteBalance) noexcept;
[[nodiscard]] std::string_view colorSpaceName(uint16_t colorSpace) noexcept;
// The flags of a Flash value, in English, such as "Fired, auto mode, red-eye reduction"
[[nodiscard]] std::string flashDescription(uint16_t flash);

} // namespace ImageMetadata
