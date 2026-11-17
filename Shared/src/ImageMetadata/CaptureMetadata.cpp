#include "ImageMetadata/CaptureMetadata.h"

#include <algorithm>
#include <ranges>
#include <set>
#include <utility>
#include <variant>
#include <vector>

namespace ImageMetadata {

namespace {

// "YYYY:MM:DD HH:MM:SS", the fraction of a second as digits, and the offset as "+HH:MM"; empty unless dateTime parses
std::optional<CaptureTime> parseCaptureTime(const QString& dateTime, const QString& subSeconds, const QString& utcOffset)
{
	if (dateTime.size() < 19 || dateTime[10] != u' ')
		return std::nullopt;

	CaptureTime captureTime{
		.date = QDate::fromString(dateTime.first(10), u"yyyy:MM:dd"),
		.time = QTime::fromString(dateTime.sliced(11, 8), u"HH:mm:ss"),
	};
	if (!captureTime.date.isValid() || !captureTime.time.isValid())
		return std::nullopt;

	if (!subSeconds.isEmpty() && std::ranges::all_of(subSeconds, [](QChar c) { return c.isDigit(); }))
		captureTime.time = captureTime.time.addMSecs(subSeconds.first(std::min<qsizetype>(subSeconds.size(), 3)).leftJustified(3, u'0').toInt());

	if (utcOffset.size() == 6 && (utcOffset[0] == u'+' || utcOffset[0] == u'-') && utcOffset[3] == u':')
	{
		bool hoursValid = false, minutesValid = false;
		const int hours = utcOffset.sliced(1, 2).toInt(&hoursValid), minutes = utcOffset.sliced(4, 2).toInt(&minutesValid);
		if (hoursValid && minutesValid)
			captureTime.utcOffsetSeconds = (utcOffset[0] == u'-' ? -1 : 1) * (hours * 3600 + minutes * 60);
	}
	return captureTime;
}

double toDouble(ExifRational rational) noexcept
{
	return static_cast<double>(rational.numerator) / static_cast<double>(rational.denominator);
}

} // namespace

CaptureMetadata takeCaptureMetadata(ExifTags& tags)
{
	using enum ExifDirectory;
	std::set<std::pair<ExifDirectory, uint16_t>> taken;

	const auto valueOf = [&](ExifDirectory directory, uint16_t tag) -> std::optional<ExifValue> {
		const auto entry = std::ranges::find_if(tags.entries, [&](const ExifEntry& e) { return e.directory == directory && e.tag == tag; });
		return entry != tags.entries.end() ? std::optional{ exifEntryValue(*entry, tags.littleEndian) } : std::nullopt;
	};
	// Each reader takes the entry when it decodes
	const auto readText = [&](ExifDirectory directory, uint16_t tag) -> std::optional<QString> {
		std::optional<ExifValue> value = valueOf(directory, tag);
		QString* const text = value ? std::get_if<QString>(&*value) : nullptr;
		if (!text)
			return std::nullopt;
		taken.emplace(directory, tag);
		return std::move(*text);
	};
	const auto readInteger = [&](ExifDirectory directory, uint16_t tag) -> std::optional<int64_t> {
		const std::optional<ExifValue> value = valueOf(directory, tag);
		const auto* const integers = value ? std::get_if<std::vector<int64_t>>(&*value) : nullptr;
		if (!integers || integers->empty())
			return std::nullopt;
		taken.emplace(directory, tag);
		return integers->front();
	};
	// Requires count values, each with a non-zero denominator
	const auto readRationals = [&](ExifDirectory directory, uint16_t tag, size_t count) -> std::optional<std::vector<ExifRational>> {
		const std::optional<ExifValue> value = valueOf(directory, tag);
		const auto* const rationals = value ? std::get_if<std::vector<ExifRational>>(&*value) : nullptr;
		if (!rationals || rationals->size() < count || std::ranges::contains(*rationals | std::views::take(count), 0, &ExifRational::denominator))
			return std::nullopt;
		taken.emplace(directory, tag);
		return *rationals;
	};
	const auto readRational = [&](ExifDirectory directory, uint16_t tag) -> std::optional<ExifRational> {
		const std::optional<std::vector<ExifRational>> rationals = readRationals(directory, tag, 1);
		return rationals ? std::optional{ rationals->front() } : std::nullopt;
	};
	const auto readCode = [&](ExifDirectory directory, uint16_t tag) -> std::optional<uint16_t> {
		const std::optional<int64_t> code = readInteger(directory, tag);
		return code ? std::optional{ static_cast<uint16_t>(*code) } : std::nullopt;
	};
	const auto setText = [&](QString& field, ExifDirectory directory, uint16_t tag) {
		if (std::optional<QString> text = readText(directory, tag))
			field = *std::move(text);
	};

	CaptureMetadata metadata;
	setText(metadata.description, Ifd0, 0x010E); // ImageDescription
	setText(metadata.make, Ifd0, 0x010F);
	setText(metadata.model, Ifd0, 0x0110);
	setText(metadata.software, Ifd0, 0x0131);
	setText(metadata.artist, Ifd0, 0x013B);
	setText(metadata.copyright, Ifd0, 0x8298);
	metadata.orientation = readCode(Ifd0, 0x0112);

	setText(metadata.bodySerial, Exif, 0xA431); // BodySerialNumber
	setText(metadata.lensMake, Exif, 0xA433);
	setText(metadata.lens, Exif, 0xA434); // LensModel
	setText(metadata.lensSerial, Exif, 0xA435);

	if (const std::optional<QString> dateTime = readText(Exif, 0x9003)) // DateTimeOriginal
	{
		metadata.captureTime = parseCaptureTime(*dateTime, readText(Exif, 0x9291).value_or(QString{}), readText(Exif, 0x9011).value_or(QString{}));
		if (!metadata.captureTime)
			std::erase_if(taken, [](const auto& key) { return key.first == Exif && (key.second == 0x9003 || key.second == 0x9291 || key.second == 0x9011); });
	}

	metadata.exposureTime = readRational(Exif, 0x829A);
	if (const std::optional<ExifRational> fNumber = readRational(Exif, 0x829D))
		metadata.fNumber = toDouble(*fNumber);
	if (const std::optional<int64_t> iso = readInteger(Exif, 0x8827); iso && *iso >= 0) // PhotographicSensitivity
		metadata.iso = static_cast<uint32_t>(*iso);
	metadata.exposureBias = readRational(Exif, 0x9204);
	if (const std::optional<ExifRational> focalLength = readRational(Exif, 0x920A))
		metadata.focalLength = toDouble(*focalLength);
	if (const std::optional<int64_t> focalLength35mm = readInteger(Exif, 0xA405); focalLength35mm && *focalLength35mm > 0) // 0: unknown
		metadata.focalLength35mm = static_cast<uint32_t>(*focalLength35mm);

	metadata.exposureProgram = readCode(Exif, 0x8822);
	metadata.meteringMode = readCode(Exif, 0x9207);
	metadata.flash = readCode(Exif, 0x9209);
	metadata.whiteBalance = readCode(Exif, 0xA403);
	metadata.colorSpace = readCode(Exif, 0xA001);

	// Degrees, minutes and seconds, negated for the reference given
	const auto readCoordinate = [&](uint16_t referenceTag, uint16_t valueTag, QChar negativeReference) -> std::optional<double> {
		const std::optional<std::vector<ExifRational>> parts = readRationals(Gps, valueTag, 3);
		if (!parts)
			return std::nullopt;

		const double degrees = toDouble((*parts)[0]) + toDouble((*parts)[1]) / 60 + toDouble((*parts)[2]) / 3600;
		const std::optional<QString> reference = readText(Gps, referenceTag);
		return reference && reference->startsWith(negativeReference) ? -degrees : degrees;
	};
	const std::optional<double> latitude = readCoordinate(0x01, 0x02, u'S');
	const std::optional<double> longitude = readCoordinate(0x03, 0x04, u'W');
	if (latitude && longitude)
	{
		metadata.gps = GpsPosition{ .latitude = *latitude, .longitude = *longitude };
		if (const std::optional<ExifRational> altitude = readRational(Gps, 0x06))
			metadata.gps->altitude = readInteger(Gps, 0x05) == 1 ? -toDouble(*altitude) : toDouble(*altitude); // 1: below sea level
	}
	else
	{
		std::erase_if(taken, [](const auto& key) { return key.first == Gps; });
	}

	std::erase_if(tags.entries, [&](const ExifEntry& entry) { return taken.contains({ entry.directory, entry.tag }); });
	return metadata;
}

std::string_view orientationName(uint16_t orientation) noexcept
{
	switch (orientation)
	{
	case 1: return "Normal";
	case 2: return "Mirror horizontal";
	case 3: return "Rotate 180°";
	case 4: return "Mirror vertical";
	case 5: return "Mirror horizontal and rotate 270° clockwise";
	case 6: return "Rotate 90° clockwise";
	case 7: return "Mirror horizontal and rotate 90° clockwise";
	case 8: return "Rotate 270° clockwise";
	default: return {};
	}
}

std::string_view exposureProgramName(uint16_t exposureProgram) noexcept
{
	switch (exposureProgram)
	{
	case 0: return "Not defined";
	case 1: return "Manual";
	case 2: return "Normal program";
	case 3: return "Aperture priority";
	case 4: return "Shutter priority";
	case 5: return "Creative program";
	case 6: return "Action program";
	case 7: return "Portrait mode";
	case 8: return "Landscape mode";
	default: return {};
	}
}

std::string_view meteringModeName(uint16_t meteringMode) noexcept
{
	switch (meteringMode)
	{
	case 0: return "Unknown";
	case 1: return "Average";
	case 2: return "Center-weighted average";
	case 3: return "Spot";
	case 4: return "Multi-spot";
	case 5: return "Pattern";
	case 6: return "Partial";
	case 255: return "Other";
	default: return {};
	}
}

std::string_view whiteBalanceName(uint16_t whiteBalance) noexcept
{
	switch (whiteBalance)
	{
	case 0: return "Auto";
	case 1: return "Manual";
	default: return {};
	}
}

std::string_view colorSpaceName(uint16_t colorSpace) noexcept
{
	switch (colorSpace)
	{
	case 1: return "sRGB";
	case 2: return "Adobe RGB"; // not in the specification, but written by cameras
	case 0xFFFF: return "Uncalibrated";
	default: return {};
	}
}

std::string flashDescription(uint16_t flash)
{
	// Bit 0: fired; bits 1-2: return light; bits 3-4: mode; bit 5: no flash function; bit 6: red-eye reduction
	if (flash & 0x20)
		return "No flash function";

	std::string description = flash & 0x01 ? "Fired" : "Did not fire";
	switch ((flash >> 3) & 0x03)
	{
	case 1: description += ", compulsory"; break;
	case 2: description += ", suppressed"; break;
	case 3: description += ", auto mode"; break;
	default: break;
	}
	switch ((flash >> 1) & 0x03)
	{
	case 2: description += ", return light not detected"; break;
	case 3: description += ", return light detected"; break;
	default: break;
	}
	if (flash & 0x40)
		description += ", red-eye reduction";
	return description;
}

} // namespace ImageMetadata
