#include "ImageDecoding/ColorManagement.h"

#include "3rdparty/catch2/catch.hpp"

#include <QByteArray>
#include <QColorSpace>
#include <QImage>

DISABLE_COMPILER_WARNINGS
#include <lcms2.h>
RESTORE_COMPILER_WARNINGS

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

using ImageDecoding::adobeRgbIccProfile;
using ImageDecoding::convertToSrgb;

namespace {

constexpr QRgb Saturated = qRgb(200, 30, 40);

using ProfilePtr = std::unique_ptr<void, decltype(&cmsCloseProfile)>;
using ToneCurvePtr = std::unique_ptr<cmsToneCurve, decltype(&cmsFreeToneCurve)>;

[[nodiscard]] QByteArray saved(cmsHPROFILE profile)
{
	cmsUInt32Number size = 0;
	REQUIRE(cmsSaveProfileToMem(profile, nullptr, &size));
	QByteArray icc{ static_cast<qsizetype>(size), Qt::Uninitialized };
	REQUIRE(cmsSaveProfileToMem(profile, icc.data(), &size));
	return icc;
}

[[nodiscard]] QByteArray grayProfile(double gamma)
{
	const ToneCurvePtr curve{ cmsBuildGamma(nullptr, gamma), &cmsFreeToneCurve };
	const ProfilePtr profile{ cmsCreateGrayProfile(cmsD50_xyY(), curve.get()), &cmsCloseProfile };
	return saved(profile.get());
}

// sRGB with its primaries rotated, red being sRGB's green, and a tabulated tone curve that dips once: Qt rejects such a curve
[[nodiscard]] QByteArray rotatedPrimariesWithDippingCurve()
{
	std::vector<cmsUInt16Number> table(1024);
	for (size_t i = 0; i < table.size(); ++i)
	{
		const double v = static_cast<double>(i) / (table.size() - 1);
		table[i] = static_cast<cmsUInt16Number>(std::lround(65535 * (v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4))));
	}
	table[256] = table[255] - 700;

	const ToneCurvePtr curve{ cmsBuildTabulatedToneCurve16(nullptr, static_cast<cmsUInt32Number>(table.size()), table.data()), &cmsFreeToneCurve };
	cmsToneCurve* const curves[]{ curve.get(), curve.get(), curve.get() };
	const cmsCIExyY d65{ 0.3127, 0.3290, 1.0 };
	const cmsCIExyYTRIPLE rotated{ { 0.30, 0.60, 1.0 }, { 0.15, 0.06, 1.0 }, { 0.64, 0.33, 1.0 } };
	const ProfilePtr profile{ cmsCreateRGBProfile(&d65, &rotated, curves), &cmsCloseProfile };
	return saved(profile.get());
}

[[nodiscard]] QImage filled(QImage::Format format, QRgb color, QSize size = { 4, 2 })
{
	QImage image{ size, QImage::Format_ARGB32 };
	image.fill(color);
	return image.convertToFormat(format);
}

[[nodiscard]] bool isNear(QRgb actual, QRgb expected, int tolerance = 2)
{
	return std::abs(qRed(actual) - qRed(expected)) <= tolerance && std::abs(qGreen(actual) - qGreen(expected)) <= tolerance
		&& std::abs(qBlue(actual) - qBlue(expected)) <= tolerance && std::abs(qAlpha(actual) - qAlpha(expected)) <= tolerance;
}

// Qt's conversion, which handles Adobe RGB correctly
[[nodiscard]] QRgb fromAdobeRgbByQt(QRgb color)
{
	QImage image = filled(QImage::Format_ARGB32, color, { 1, 1 });
	image.setColorSpace(QColorSpace::AdobeRgb);
	image.convertToColorSpace(QColorSpace::SRgb);
	return image.pixel(0, 0);
}

} // namespace

TEST_CASE("convertToSrgb: from the profile to sRGB, as Qt converts Adobe RGB", "[colormanagement]")
{
	QImage image = filled(QImage::Format_RGB32, Saturated);
	convertToSrgb(image, adobeRgbIccProfile());
	REQUIRE(image.format() == QImage::Format_RGB32);
	REQUIRE(image.colorSpace() == QColorSpace{ QColorSpace::SRgb });
	REQUIRE_FALSE(isNear(image.pixel(0, 0), Saturated));
	REQUIRE(isNear(image.pixel(0, 0), fromAdobeRgbByQt(Saturated)));
	REQUIRE(qAlpha(image.pixel(0, 0)) == 255);
}

TEST_CASE("convertToSrgb: a profile Qt rejects", "[colormanagement]")
{
	const QByteArray profile = rotatedPrimariesWithDippingCurve();
	REQUIRE_FALSE(QColorSpace::fromIccProfile(profile).isValid());

	QImage image = filled(QImage::Format_RGB32, qRgb(255, 0, 0));
	convertToSrgb(image, profile);
	const QRgb converted = image.pixel(0, 0);
	CAPTURE(qRed(converted), qGreen(converted), qBlue(converted));
	REQUIRE(qGreen(converted) > 200);
	REQUIRE(qRed(converted) < 60);
	REQUIRE(qBlue(converted) < 60);
}

TEST_CASE("convertToSrgb: an sRGB profile converts nothing", "[colormanagement]")
{
	const ProfilePtr lcmsSrgb{ cmsCreate_sRGBProfile(), &cmsCloseProfile };
	for (const QByteArray& profile : { QColorSpace{ QColorSpace::SRgb }.iccProfile(), saved(lcmsSrgb.get()) })
	{
		QImage image = filled(QImage::Format_RGB32, Saturated);
		const QImage original = image;
		convertToSrgb(image, profile);
		REQUIRE(image == original);
	}
}

TEST_CASE("convertToSrgb: grayscale stays grayscale", "[colormanagement]")
{
	QImage image = filled(QImage::Format_Grayscale8, qRgb(128, 128, 128));
	convertToSrgb(image, grayProfile(1.8));
	REQUIRE(image.format() == QImage::Format_Grayscale8);
	// 128 at gamma 1.8 is sRGB's 147
	REQUIRE(std::abs(qGray(image.pixel(0, 0)) - 147) <= 2);
}

TEST_CASE("convertToSrgb: alpha and 16-bit channels survive", "[colormanagement]")
{
	const QRgb translucent = qRgba(200, 30, 40, 128);

	QImage straight = filled(QImage::Format_ARGB32, translucent);
	convertToSrgb(straight, adobeRgbIccProfile());
	REQUIRE(straight.format() == QImage::Format_ARGB32);
	REQUIRE(isNear(straight.pixel(0, 0), fromAdobeRgbByQt(translucent)));

	QImage premultiplied = filled(QImage::Format_ARGB32_Premultiplied, translucent);
	convertToSrgb(premultiplied, adobeRgbIccProfile());
	REQUIRE(premultiplied.format() == QImage::Format_ARGB32_Premultiplied);
	// pixel() is premultiplied here; colors stored at half alpha lose a bit of precision
	REQUIRE(isNear(premultiplied.convertToFormat(QImage::Format_ARGB32).pixel(0, 0), fromAdobeRgbByQt(translucent), 3));

	QImage deep = filled(QImage::Format_RGBA64, Saturated);
	convertToSrgb(deep, adobeRgbIccProfile());
	REQUIRE(deep.format() == QImage::Format_RGBA64);
	REQUIRE(isNear(deep.pixel(0, 0), fromAdobeRgbByQt(Saturated)));
}

TEST_CASE("convertToSrgb: other RGB layouts convert through 32 bits", "[colormanagement]")
{
	QImage image = filled(QImage::Format_RGB16, Saturated);
	const QRgb stored = image.pixel(0, 0); // RGB16 rounds the color
	convertToSrgb(image, adobeRgbIccProfile());
	REQUIRE(image.format() == QImage::Format_RGB32);
	REQUIRE(isNear(image.pixel(0, 0), fromAdobeRgbByQt(stored)));

	QImage gray = filled(QImage::Format_Grayscale8, qRgb(128, 128, 128));
	convertToSrgb(gray, adobeRgbIccProfile());
	REQUIRE(gray.format() == QImage::Format_RGB32);
}

TEST_CASE("convertToSrgb: pixels the profile does not describe stay as they are", "[colormanagement]")
{
	const auto unchanged = [](QImage image, const QByteArray& profile) {
		const QImage original = image;
		convertToSrgb(image, profile);
		return image == original;
	};

	REQUIRE(unchanged(filled(QImage::Format_RGB32, Saturated), {}));
	REQUIRE(unchanged(filled(QImage::Format_RGB32, Saturated), "not a profile"));
	REQUIRE(unchanged(filled(QImage::Format_RGB32, Saturated), grayProfile(1.8)));
	REQUIRE(unchanged(filled(QImage::Format_CMYK8888, Saturated), adobeRgbIccProfile()));
	REQUIRE(unchanged(QImage{}, adobeRgbIccProfile()));
}

TEST_CASE("convertToSrgb: a large image converts in bands, all alike", "[colormanagement]")
{
	QImage image{ 700, 300, QImage::Format_RGB32 };
	for (int x = 0; x < image.width(); ++x)
		image.setPixel(x, 0, qRgb(x % 256, (x * 7) % 256, (x * 13) % 256));
	for (int y = 1; y < image.height(); ++y)
		std::memcpy(image.scanLine(y), image.constScanLine(0), static_cast<size_t>(image.bytesPerLine()));

	QImage firstRow = image.copy(0, 0, image.width(), 1);
	convertToSrgb(firstRow, adobeRgbIccProfile());
	convertToSrgb(image, adobeRgbIccProfile());
	for (int y = 0; y < image.height(); ++y)
	{
		CAPTURE(y);
		REQUIRE(std::memcmp(image.constScanLine(y), firstRow.constScanLine(0), static_cast<size_t>(firstRow.bytesPerLine())) == 0);
	}
}

TEST_CASE("adobeRgbIccProfile: Qt reads it as Adobe RGB", "[colormanagement]")
{
	REQUIRE(QColorSpace::fromIccProfile(adobeRgbIccProfile()) == QColorSpace{ QColorSpace::AdobeRgb });
}
