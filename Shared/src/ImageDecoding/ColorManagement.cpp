#include "ImageDecoding/ColorManagement.h"
#include "Core/CpuThreadPool.h"

DISABLE_COMPILER_WARNINGS
#include <QColor>
#include <QColorSpace>
#include <QPixelFormat>
#include <QtGlobal>
#include <lcms2.h>
RESTORE_COMPILER_WARNINGS

#include <algorithm>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

static_assert(Q_BYTE_ORDER == Q_LITTLE_ENDIAN, "TYPE_BGRA_8 lays out Format_RGB32 on little-endian machines only");

namespace ImageDecoding {

namespace {

using ProfilePtr = std::unique_ptr<void, decltype(&cmsCloseProfile)>;
using TransformPtr = std::unique_ptr<void, decltype(&cmsDeleteTransform)>;
using ToneCurvePtr = std::unique_ptr<cmsToneCurve, decltype(&cmsFreeToneCurve)>;

struct Transform
{
	TransformPtr handle;
	QImage::Format inputFormat;    // the image is converted to it first when it differs
	QImage::Format outputFormat;   // and back to it afterwards, when that differs
	bool toRgb32;                  // CMYK: the result is a new Format_RGB32 image, not converted in place
	bool identity;                 // sRGB reproduces the profile: nothing to convert
};

// Premultiplied pixels convert with straight alpha: lcms 2.19's premultiplied formats produce wrong colors
std::optional<QImage::Format> straightAlphaFormat(QImage::Format format)
{
	switch (format)
	{
	case QImage::Format_ARGB32_Premultiplied: return QImage::Format_ARGB32;
	case QImage::Format_RGBA8888_Premultiplied: return QImage::Format_RGBA8888;
	case QImage::Format_RGBA64_Premultiplied: return QImage::Format_RGBA64;
	default: return std::nullopt;
	}
}

// lcms's layout of a Qt format in the profile's color model; empty when the pixels do not fit it
std::optional<cmsUInt32Number> lcmsFormat(QImage::Format format, cmsColorSpaceSignature colorModel)
{
	switch (colorModel)
	{
	case cmsSigRgbData:
		switch (format)
		{
		case QImage::Format_RGB32:
		case QImage::Format_ARGB32: return TYPE_BGRA_8;
		case QImage::Format_RGB888: return TYPE_RGB_8;
		case QImage::Format_RGBX8888:
		case QImage::Format_RGBA8888: return TYPE_RGBA_8;
		case QImage::Format_RGBX64:
		case QImage::Format_RGBA64: return TYPE_RGBA_16;
		default: return std::nullopt;
		}
	case cmsSigGrayData:
		switch (format)
		{
		case QImage::Format_Grayscale8: return TYPE_GRAY_8;
		case QImage::Format_Grayscale16: return TYPE_GRAY_16;
		default: return std::nullopt;
		}
	case cmsSigCmykData:
		return format == QImage::Format_CMYK8888 ? std::optional<cmsUInt32Number>{ TYPE_CMYK_8 } : std::nullopt;
	default:
		return std::nullopt;
	}
}

// Rows in parallel bands: lcms converts on the calling thread only
void transformPixels(const Transform& transform, const QImage& source, QImage& target)
{
	// Pointers taken once: scanLine() writes to the image, so the bands must not call it. Target first: in place, it may detach.
	uchar* const targetBits = target.bits();
	const uchar* const sourceBits = source.constBits();
	const qsizetype sourceStride = source.bytesPerLine(), targetStride = target.bytesPerLine();
	const int width = source.width(), height = source.height();
	const int rowsPerBand = std::max(1, (1 << 16) / width);
	const int bandCount = (height + rowsPerBand - 1) / rowsPerBand;
	cpuParallelFor(static_cast<size_t>(bandCount), [&](size_t band) {
		const int y = static_cast<int>(band) * rowsPerBand;
		cmsDoTransformLineStride(transform.handle.get(), sourceBits + y * sourceStride, targetBits + y * targetStride, static_cast<cmsUInt32Number>(width),
			static_cast<cmsUInt32Number>(std::min(rowsPerBand, height - y)), static_cast<cmsUInt32Number>(sourceStride), static_cast<cmsUInt32Number>(targetStride), 0, 0);
	});
}

// Every combination of 17 levels per channel, in format
QImage identitySample(QImage::Format format)
{
	constexpr int Levels = 17;
	const auto level = [](int index) { return std::min(255, index * 16); };
	QImage sample{ Levels * Levels * Levels, 1, QImage::Format_RGB32 };
	for (int i = 0; i < sample.width(); ++i)
		sample.setPixel(i, 0, qRgb(level(i / (Levels * Levels)), level(i / Levels % Levels), level(i % Levels)));
	return sample.convertToFormat(format);
}

// Within one 8-bit level on every channel. Runs on the calling thread: it is called under transformFor's lock.
bool convertsToItself(const Transform& transform)
{
	const QImage sample = identitySample(transform.inputFormat);
	QImage converted{ sample.size(), sample.format() };
	cmsDoTransform(transform.handle.get(), sample.constBits(), converted.bits(), static_cast<cmsUInt32Number>(sample.width()));
	for (int x = 0; x < sample.width(); ++x)
	{
		const QRgba64 a = sample.pixelColor(x, 0).rgba64(), b = converted.pixelColor(x, 0).rgba64();
		if (std::abs(a.red() - b.red()) > 257 || std::abs(a.green() - b.green()) > 257 || std::abs(a.blue() - b.blue()) > 257)
			return false;
	}
	return true;
}

cmsHPROFILE srgbProfile()
{
	static const ProfilePtr profile{ cmsCreate_sRGBProfile(), &cmsCloseProfile };
	return profile.get();
}

// sRGB's tone curve over gray: grayscale converts to it and stays grayscale
cmsHPROFILE srgbGrayProfile()
{
	static const ProfilePtr profile = [] {
		const cmsFloat64Number srgbCurve[]{ 2.4, 1 / 1.055, 0.055 / 1.055, 1 / 12.92, 0.04045 };
		const ToneCurvePtr curve{ cmsBuildParametricToneCurve(nullptr, 4, srgbCurve), &cmsFreeToneCurve };
		return ProfilePtr{ cmsCreateGrayProfile(cmsD50_xyY(), curve.get()), &cmsCloseProfile };
	}();
	return profile.get();
}

// Null when the profile is unusable or the pixel format does not match it
std::shared_ptr<const Transform> createTransform(const QByteArray& iccProfile, QImage::Format format)
{
	const ProfilePtr profile{ cmsOpenProfileFromMem(iccProfile.constData(), static_cast<cmsUInt32Number>(iccProfile.size())), &cmsCloseProfile };
	if (!profile)
		return nullptr;

	const cmsColorSpaceSignature colorModel = cmsGetColorSpace(profile.get());
	const std::optional<QImage::Format> straightAlpha = straightAlphaFormat(format);
	QImage::Format inputFormat = straightAlpha.value_or(format);
	// Any other layout of RGB pixels, gray ones included, is converted to 32 bits first
	if (colorModel == cmsSigRgbData && !lcmsFormat(inputFormat, colorModel) && format != QImage::Format_CMYK8888)
		inputFormat = QImage::toPixelFormat(format).alphaUsage() == QPixelFormat::UsesAlpha ? QImage::Format_ARGB32 : QImage::Format_RGB32;

	const std::optional<cmsUInt32Number> input = lcmsFormat(inputFormat, colorModel);
	if (!input)
		return nullptr;

	const bool toRgb32 = colorModel == cmsSigCmykData;
	const cmsUInt32Number output = toRgb32 ? TYPE_BGRA_8 : *input;
	// NOCACHE: one transform serves several threads at once
	const cmsUInt32Number flags = cmsFLAGS_NOCACHE | (T_EXTRA(*input) != 0 ? cmsFLAGS_COPY_ALPHA : 0);
	TransformPtr handle{ cmsCreateTransform(profile.get(), *input, colorModel == cmsSigGrayData ? srgbGrayProfile() : srgbProfile(), output, INTENT_PERCEPTUAL, flags),
		&cmsDeleteTransform };
	if (!handle)
		return nullptr;

	auto transform = std::make_shared<Transform>(Transform{ std::move(handle), inputFormat, straightAlpha ? format : inputFormat, toRgb32, false });
	transform->identity = !toRgb32 && convertsToItself(*transform);
	return transform;
}

std::shared_ptr<const Transform> transformFor(const QByteArray& iccProfile, QImage::Format format)
{
	static std::mutex mutex;
	static std::map<std::pair<QByteArray, QImage::Format>, std::shared_ptr<const Transform>> cache;

	const std::lock_guard lock{ mutex };
	auto key = std::make_pair(iccProfile, format);
	if (const auto cached = cache.find(key); cached != cache.end())
		return cached->second;

	// Bounded: a session meets few distinct profiles
	if (cache.size() >= 64)
		cache.clear();
	return cache.emplace(std::move(key), createTransform(iccProfile, format)).first->second;
}

} // namespace

const QByteArray& adobeRgbIccProfile()
{
	static const QByteArray bytes = [] {
		const cmsCIExyY d65{ 0.3127, 0.3290, 1.0 };
		const cmsCIExyYTRIPLE primaries{ { 0.64, 0.33, 1.0 }, { 0.21, 0.71, 1.0 }, { 0.15, 0.06, 1.0 } };
		const ToneCurvePtr gamma{ cmsBuildGamma(nullptr, 563.0 / 256.0), &cmsFreeToneCurve };
		cmsToneCurve* const curves[]{ gamma.get(), gamma.get(), gamma.get() };
		const ProfilePtr profile{ cmsCreateRGBProfile(&d65, &primaries, curves), &cmsCloseProfile };

		const std::unique_ptr<cmsMLU, decltype(&cmsMLUfree)> description{ cmsMLUalloc(nullptr, 1), &cmsMLUfree };
		cmsMLUsetASCII(description.get(), "en", "US", "Adobe RGB (1998)");
		cmsWriteTag(profile.get(), cmsSigProfileDescriptionTag, description.get());

		cmsUInt32Number size = 0;
		cmsSaveProfileToMem(profile.get(), nullptr, &size);
		QByteArray icc{ static_cast<qsizetype>(size), Qt::Uninitialized };
		cmsSaveProfileToMem(profile.get(), icc.data(), &size);
		return icc;
	}();
	return bytes;
}

void convertToSrgb(QImage& image, const QByteArray& iccProfile)
{
	if (iccProfile.isEmpty() || image.isNull())
		return;

	const std::shared_ptr<const Transform> transform = transformFor(iccProfile, image.format());
	if (!transform || transform->identity)
		return;

	if (image.format() != transform->inputFormat)
		image.convertTo(transform->inputFormat);

	if (transform->toRgb32)
	{
		QImage converted{ image.size(), QImage::Format_RGB32 };
		converted.fill(Qt::black); // the alpha byte, which the transform does not write
		converted.setDevicePixelRatio(image.devicePixelRatio());
		transformPixels(*transform, image, converted);
		image = std::move(converted);
	}
	else
	{
		transformPixels(*transform, image, image);
		if (image.format() != transform->outputFormat)
			image.convertTo(transform->outputFormat);
	}

	image.setColorSpace(QColorSpace::SRgb);
}

} // namespace ImageDecoding
