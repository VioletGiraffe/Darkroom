#include "Windows/ImageInfoRows.h"

#include "3rdparty/catch2/catch.hpp"

#include <QByteArray>
#include <QString>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

using namespace ImageInfoRows;
using ImageMetadata::ExifRational;
using ImageMetadata::ExifValue;

namespace {

[[nodiscard]] std::vector<QString> titles(const std::vector<Group>& groups)
{
	std::vector<QString> titles;
	for (const Group& group : groups)
		titles.push_back(group.title);
	return titles;
}

[[nodiscard]] const Row* findRow(const std::vector<Group>& groups, const QString& title, const QString& key)
{
	const auto group = std::ranges::find(groups, title, &Group::title);
	if (group == groups.end())
		return nullptr;
	const auto row = std::ranges::find(group->rows, key, &Row::key);
	return row != group->rows.end() ? &*row : nullptr;
}

} // namespace

TEST_CASE("exposureTimeText: a fraction below a second when one is near, else seconds", "[imageinfo]")
{
	CHECK(exposureTimeText({ 1, 250 }) == "1/250 s");
	CHECK(exposureTimeText({ 10, 1250 }) == "1/125 s");
	CHECK(exposureTimeText({ 1, 3 }) == "1/3 s");
	CHECK(exposureTimeText({ 3, 10 }) == "0.3 s");
	CHECK(exposureTimeText({ 2, 1 }) == "2 s");
	CHECK(exposureTimeText({ 25, 10 }) == "2.5 s");
}

TEST_CASE("fNumberText and exposureBiasText", "[imageinfo]")
{
	CHECK(fNumberText(2.8) == "f/2.8");
	CHECK(fNumberText(11.0) == "f/11");
	CHECK(fNumberText(1.4) == "f/1.4");

	CHECK(exposureBiasText({ -2, 3 }) == "-0.67 EV");
	CHECK(exposureBiasText({ 1, 1 }) == "+1 EV");
	CHECK(exposureBiasText({ 0, 1 }) == "0 EV");
}

TEST_CASE("lensSpecificationText: a range shows once when both ends match; a zoom's one known f-number names its end", "[imageinfo]")
{
	using ImageMetadata::LensSpecification;
	CHECK(lensSpecificationText(LensSpecification{ 90.0, 90.0, 2.8, 2.8 }) == "90 mm f/2.8");
	CHECK(lensSpecificationText(LensSpecification{ 24.0, 70.0, 4.0, 4.0 }) == "24–70 mm f/4");
	CHECK(lensSpecificationText(LensSpecification{ 18.0, 55.0, 3.5, 5.6 }) == "18–55 mm f/3.5–5.6");
	CHECK(lensSpecificationText(LensSpecification{ 24.0, 120.0, 4.0, std::nullopt }) == "24–120 mm f/4 at 24 mm");
	CHECK(lensSpecificationText(LensSpecification{ 4.5, 9.0, std::nullopt, 5.6 }) == "4.5–9 mm f/5.6 at 9 mm");
	CHECK(lensSpecificationText(LensSpecification{ 35.0, 35.0, std::nullopt, 2.0 }) == "35 mm f/2");
	CHECK(lensSpecificationText(LensSpecification{ 35.0, 35.0, std::nullopt, std::nullopt }) == "35 mm");
}

TEST_CASE("exifValueText: lists cut after their first values, bytes as hex while short", "[imageinfo]")
{
	CHECK(exifValueText(ExifValue{ QString{ "Text" } }) == "Text");
	CHECK(exifValueText(ExifValue{ std::vector<int64_t>{ 2, 3, 0, 0 } }) == "2, 3, 0, 0");
	CHECK(exifValueText(ExifValue{ std::vector<int64_t>(10, 7) }) == "7, 7, 7, 7, 7, 7, 7, 7 ... (10 values)");
	CHECK(exifValueText(ExifValue{ std::vector<ExifRational>{ { 1, 250 }, { 50, 1 } } }) == "1/250, 50");
	CHECK(exifValueText(ExifValue{ std::vector<double>{ 1.5 } }) == "1.5");
	CHECK(exifValueText(ExifValue{ QByteArray{ "\x01\x02\xAB", 3 } }) == "01 02 AB");
	CHECK(exifValueText(ExifValue{ QByteArray(17, 'x') }) == "17 bytes");
}

TEST_CASE("groups: the summary groups, then the other tags by directory", "[imageinfo]")
{
	ImageMetadata::ImageInfo info{ .fileSize = 1234, .format = "jpeg", .uprightSize = { 6000, 4000 } };
	info.capture.make = "Canon";
	info.capture.model = "Canon EOS R5";
	info.capture.lensMake = "Canon";
	info.capture.lens = "RF50mm";
	info.capture.lensSpecification = ImageMetadata::LensSpecification{ 50.0, 50.0, 1.8, 1.8 };
	info.capture.exposureTime = ExifRational{ 1, 250 };
	info.capture.focalLength = 50.0;
	info.capture.focalLength35mm = 50;
	info.capture.meteringMode = 5;
	info.capture.gps = ImageMetadata::GpsPosition{ .latitude = -33.5, .longitude = 151.25, .altitude = -2.0 };
	info.texts = { { "Comment", "Hello" } };
	info.otherExif.entries = {
		{ ImageMetadata::ExifDirectory::Exif, 0xA434, 7, 1, QByteArray{ "\x05", 1 } },
		{ ImageMetadata::ExifDirectory::Exif, 0x927C, 7, 4, QByteArray{ "\x01\x02\x03\x04", 4 } }, // MakerNote, listed before the lower tag
		{ ImageMetadata::ExifDirectory::Ifd0, 0xFFFE, 7, 1, QByteArray{ "\x06", 1 } },
	};

	const std::vector<Group> groups = ImageInfoRows::groups("C:/Photos/IMG_1.jpg", info);
	CHECK(titles(groups) == std::vector<QString>{ "File", "Image", "Camera", "Exposure", "Location", "Texts", "Other tags: IFD0", "Other tags: Exif" });

	REQUIRE(findRow(groups, "File", "Name"));
	CHECK(findRow(groups, "File", "Name")->value == "IMG_1.jpg");
	CHECK(findRow(groups, "Image", "Dimensions")->value == "6000 × 4000 (24.0 MP)");
	CHECK_FALSE(findRow(groups, "Image", "EXIF"));
	CHECK(findRow(groups, "Camera", "Camera")->value == "Canon EOS R5");
	CHECK(findRow(groups, "Camera", "Lens")->value == "Canon RF50mm");
	CHECK(findRow(groups, "Camera", "Lens specification")->value == "50 mm f/1.8");
	CHECK(findRow(groups, "Exposure", "Shutter speed")->value == "1/250 s");
	CHECK(findRow(groups, "Exposure", "Focal length")->value == "50 mm (50 mm in 35 mm format)");
	CHECK(findRow(groups, "Exposure", "Metering mode")->value == "Pattern");
	CHECK(findRow(groups, "Location", "Coordinates")->value == "33.5° S, 151.25° E");
	CHECK(findRow(groups, "Location", "Altitude")->value == "-2 m");
	CHECK(findRow(groups, "Texts", "Comment")->value == "Hello");

	const Group& exif = groups.back();
	REQUIRE(exif.rows.size() == 2);
	CHECK(exif.rows[0].key == "MakerNote");
	CHECK(exif.rows[0].value == "01 02 03 04");
	CHECK(exif.rows[0].keyToolTip == "Tag 0x927C (37500)");
	CHECK(exif.rows[1].key == "LensModel");
	CHECK(findRow(groups, "Other tags: IFD0", "Tag 0xFFFE"));

	SECTION("a lens without a name shows its specification instead")
	{
		info.capture.lens.clear();
		const std::vector<Group> unnamed = ImageInfoRows::groups("C:/Photos/IMG_1.jpg", info);
		CHECK(findRow(unnamed, "Camera", "Lens")->value == "Canon 50 mm f/1.8");
		CHECK_FALSE(findRow(unnamed, "Camera", "Lens specification"));
	}
}

TEST_CASE("groups: the color profile by its source, and a profile without a name or unreadable", "[imageinfo]")
{
	using Source = ImageMetadata::ColorProfile::Source;
	const auto profileRow = [](Source source, std::optional<QString> name) {
		const std::vector<Group> rows = groups("image.jpg", ImageMetadata::ImageInfo{ .colorProfile = ImageMetadata::ColorProfile{ source, std::move(name) } });
		const auto group = std::ranges::find(rows, QString{ "Image" }, &Group::title);
		REQUIRE(group != rows.end());
		REQUIRE_FALSE(group->rows.empty());
		return group->rows.front();
	};

	const Row embedded = profileRow(Source::Embedded, "sRGB IEC61966-2.1");
	CHECK(embedded.key == "Color profile");
	CHECK(embedded.value == "sRGB IEC61966-2.1");
	CHECK(profileRow(Source::Embedded, QString{}).value == "Unnamed");
	CHECK(profileRow(Source::Embedded, std::nullopt).value == "Unreadable");
	CHECK(profileRow(Source::ExifAdobeRgb, "Adobe RGB (1998)").value == "Adobe RGB (1998) (declared in EXIF)");

	const Row raw = profileRow(Source::CameraRaw, "Camera");
	CHECK(raw.key == "Embedded profile");
	CHECK(raw.value == "Camera");
	CHECK_FALSE(raw.keyToolTip.isEmpty());
}

TEST_CASE("groups: a file without EXIF says so; an unreadable file shows the error only", "[imageinfo]")
{
	const std::vector<Group> plain = groups("image.png", ImageMetadata::ImageInfo{ .format = "png" });
	REQUIRE(findRow(plain, "Image", "EXIF"));
	CHECK(findRow(plain, "Image", "EXIF")->value == "None");

	const std::vector<Group> failed = groups("gone.jpg", ImageMetadata::ImageInfo{ .error = "Not found" });
	CHECK(titles(failed) == std::vector<QString>{ "File" });
	CHECK(findRow(failed, "File", "Error")->value == "Not found");
}
