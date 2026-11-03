#include "ImageEncoding/ImageEncoding.h"
#include "ImageMetadata/Exif.h"
#include "assert/advanced_assert.h"

DISABLE_COMPILER_WARNINGS
#include <QBuffer>
#include <QColorSpace>
#include <QImage>
#include <QImageWriter>
#include <QList>
#include <QMimeDatabase>
#include <QMimeType>
#include <QObject>
#include <QPainter>
#include <QSaveFile>
#include <QtGlobal>
#include <turbojpeg.h>
RESTORE_COMPILER_WARNINGS

#include <algorithm>
#include <array>
#include <memory>

namespace ImageEncoding {

namespace {

constexpr const char* JpegMimeType = "image/jpeg";

struct Format
{
	const char* mimeType;
	bool lossy;
};

constexpr std::array Formats{
	Format{ "image/avif", true },
	Format{ "image/bmp", false },
	Format{ "image/heic", true },
	Format{ "image/heif", true },
	Format{ JpegMimeType, true },
	Format{ "image/jxl", true },
	Format{ "image/png", false },
	Format{ "image/tiff", false },
	Format{ "image/webp", true },
};

QString notEnoughMemoryError(const QImage& image)
{
	return QObject::tr("Not enough memory to save a %1x%2 image").arg(image.width()).arg(image.height());
}

// image in a layout tj3Compress8() takes: RGB32, RGB888 or Grayscale8. Alpha is composed onto white. Null when out of memory.
QImage jpegCompressibleImage(const QImage& image)
{
	if (image.hasAlphaChannel())
	{
		QImage flattened{ image.size(), QImage::Format_RGB32 };
		if (flattened.isNull())
			return {};
		flattened.fill(Qt::white);
		QPainter{ &flattened }.drawImage(0, 0, image);
		return flattened;
	}

	switch (image.format())
	{
	case QImage::Format_RGB32:
	case QImage::Format_RGB888:
	case QImage::Format_Grayscale8:
		return image;
	case QImage::Format_Grayscale16:
		return image.convertToFormat(QImage::Format_Grayscale8);
	default:
		return image.convertToFormat(QImage::Format_RGB32);
	}
}

EncodedFile encodeJpeg(const QImage& image, const QByteArray& iccProfile, int quality, JpegSubsampling subsampling)
{
	const QImage source = jpegCompressibleImage(image);
	if (source.isNull())
		return { .error = notEnoughMemoryError(image) };

	const std::unique_ptr<void, decltype(&tj3Destroy)> handle{ tj3Init(TJINIT_COMPRESS), &tj3Destroy };
	const auto error = [&] { return EncodedFile{ .error = QString::fromLocal8Bit(tj3GetErrorStr(handle.get())) }; };
	if (!handle)
		return error();

	const bool gray = source.format() == QImage::Format_Grayscale8;
	const int pixelFormat = gray ? TJPF_GRAY
		: source.format() == QImage::Format_RGB888 ? TJPF_RGB
		: Q_BYTE_ORDER == Q_LITTLE_ENDIAN ? TJPF_BGRX : TJPF_XRGB; // Format_RGB32 is 0xffRRGGBB
	const int chromaSubsampling = subsampling == JpegSubsampling::Chroma444 ? TJSAMP_444 : TJSAMP_420;
	tj3Set(handle.get(), TJPARAM_QUALITY, std::clamp(quality, 1, 100));
	tj3Set(handle.get(), TJPARAM_SUBSAMP, gray ? TJSAMP_GRAY : chromaSubsampling);
	tj3Set(handle.get(), TJPARAM_OPTIMIZE, 1);
	// tj3SetICCProfile() copies the profile
	if (!iccProfile.isEmpty()
		&& tj3SetICCProfile(handle.get(), reinterpret_cast<unsigned char*>(const_cast<char*>(iccProfile.constData())), static_cast<size_t>(iccProfile.size())) != 0)
		return error();

	unsigned char* jpeg = nullptr;
	size_t jpegSize = 0;
	const int result = tj3Compress8(handle.get(), source.constBits(), source.width(), static_cast<int>(source.bytesPerLine()), source.height(), pixelFormat, &jpeg, &jpegSize);
	const std::unique_ptr<unsigned char, decltype(&tj3Free)> jpegOwner{ jpeg, &tj3Free };
	if (result != 0)
		return error();
	return { .bytes = QByteArray{ reinterpret_cast<const char*>(jpeg), static_cast<qsizetype>(jpegSize) } };
}

EncodedFile encodeWithQt(const QImage& image, const QByteArray& iccProfile, const EncodingOptions& options)
{
	QImage tagged = image;
	if (!tagged.colorSpace().isValid() && !iccProfile.isEmpty())
		tagged.setColorSpace(QColorSpace::fromIccProfile(iccProfile));

	QBuffer encoded;
	encoded.open(QIODevice::WriteOnly);
	QImageWriter writer{ &encoded, QImageWriter::imageFormatsForMimeType(options.mimeType.toLatin1()).value(0) };
	if (isLossy(options.mimeType))
		writer.setQuality(options.quality);
	if (!writer.write(tagged))
		return { .error = writer.errorString() };
	return { .bytes = encoded.data() };
}

} // namespace

QStringList writableMimeTypes()
{
	const QList<QByteArray> qtWritableTypes = QImageWriter::supportedMimeTypes();
	QStringList writableTypes;
	for (const Format& format : Formats)
	{
		if (QLatin1String(format.mimeType) == QLatin1String(JpegMimeType) || qtWritableTypes.contains(format.mimeType))
			writableTypes.push_back(QString::fromLatin1(format.mimeType));
	}
	return writableTypes;
}

QStringList offeredMimeTypes()
{
	QStringList types = writableMimeTypes();
	if (types.contains(QLatin1String("image/heic")))
		types.removeOne(QStringLiteral("image/heif"));
	return types;
}

QString writableMimeTypeOf(const QMimeType& type)
{
	const QMimeDatabase mimeDatabase;
	const QStringList writableTypes = writableMimeTypes();
	const auto listed = std::ranges::find_if(writableTypes, [&](const QString& name) { return mimeDatabase.mimeTypeForName(name) == type; });
	return listed != writableTypes.end() ? *listed : QString{};
}

bool isLossy(const QString& mimeType)
{
	const auto format = std::ranges::find_if(Formats, [&](const Format& listed) { return mimeType == QLatin1String(listed.mimeType); });
	assert_and_return_r(format != Formats.end(), false);
	return format->lossy;
}

bool takesExif(const QString& mimeType)
{
	return mimeType == QLatin1String(JpegMimeType);
}

EncodedFile encodeImage(const QImage& image, const QByteArray& iccProfile, const EncodingOptions& options, QByteArrayView sourceFile)
{
	if (!takesExif(options.mimeType))
		return encodeWithQt(image, iccProfile, options);

	EncodedFile jpeg = encodeJpeg(image, iccProfile, options.quality, options.jpegSubsampling);
	if (!jpeg.bytes.isEmpty() && !sourceFile.empty())
		jpeg.bytes = ImageMetadata::jpegWithExifApp1(jpeg.bytes, ImageMetadata::exifApp1ForOutput(sourceFile, image.size()));
	return jpeg;
}

QString writeFileAtomically(const QString& path, QByteArrayView bytes, const std::function<void()>& beforeReplacing)
{
	QSaveFile file{ path };
	if (!file.open(QIODevice::WriteOnly))
		return file.errorString();
	if (file.write(bytes.data(), bytes.size()) != bytes.size())
		return file.errorString();
	if (beforeReplacing)
		beforeReplacing();
	return file.commit() ? QString{} : file.errorString();
}

QString writeImage(const QImage& image, const QByteArray& iccProfile, const QString& targetPath, const EncodingOptions& options, QByteArrayView sourceFile)
{
	const EncodedFile encoded = encodeImage(image, iccProfile, options, sourceFile);
	return encoded.bytes.isEmpty() ? encoded.error : writeFileAtomically(targetPath, encoded.bytes, {});
}

} // namespace ImageEncoding
