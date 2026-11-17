#include "ImageMetadata/ImageInfo.h"
#include "ImageDecoding/ColorManagement.h"
#include "ImageDecoding/ImageDecoding.h"
#include "ImageDecoding/LibRawDecoder.h"
#include "ImageMetadata/EmbeddedMetadata.h"
#include "ImageMetadata/Exif.h"
#include "buffered_reader.hpp" // thin_io
#include "qt_paths.hpp"        // thin_io

DISABLE_COMPILER_WARNINGS
#include <QBuffer>
#include <QFile>
#include <QFileInfo>
#include <QImageIOHandler>
#include <QImageReader>
RESTORE_COMPILER_WARNINGS

#include <vector>

namespace ImageMetadata {

namespace {

constexpr uint16_t InterColorProfileTag = 0x8773, AsShotIccProfileTag = 0xC68F;

ColorProfile colorProfile(ColorProfile::Source source, const QByteArray& iccProfile)
{
	return { source, ImageDecoding::iccProfileDescription(iccProfile) };
}

// Leaves out IFD0's entries holding iccProfile: colorProfile shows it
void setExif(ImageInfo& info, ExifTags exif, const QByteArray& iccProfile)
{
	if (!iccProfile.isEmpty())
	{
		std::erase_if(exif.entries, [&](const ExifEntry& entry) {
			return entry.directory == ExifDirectory::Ifd0 && (entry.tag == InterColorProfileTag || entry.tag == AsShotIccProfileTag) && entry.value == iccProfile;
		});
	}
	info.capture = takeCaptureMetadata(exif);
	info.otherExif = std::move(exif);
}

ImageInfo readRaw(ImageInfo info, const QString& path, const QString& suffix)
{
	thin_io::buffered_reader file{ ImageDecoding::LibRawReadWindowSize };
	if (!file.open(thin_io::native_path(path).data()))
		return { .error = QString::fromStdString(thin_io::buffered_reader::text_for_last_error()) };

	info.format = suffix.toLatin1();
	std::optional<ImageDecoding::RawMetadata> raw = ImageDecoding::readRawMetadata(file);
	if (!raw)
		return info;

	info.uprightSize = raw->uprightSize;
	if (!raw->iccProfile.isEmpty())
		info.colorProfile = colorProfile(ColorProfile::Source::CameraRaw, raw->iccProfile);
	setExif(info, std::move(raw->exif), raw->iccProfile);

	const auto fillIn = [](QString& field, QString& decoded) {
		if (field.isEmpty())
			field = std::move(decoded);
	};
	fillIn(info.capture.lensMake, raw->lensMake);
	fillIn(info.capture.lens, raw->lens);
	fillIn(info.capture.lensSerial, raw->lensSerial);
	fillIn(info.capture.bodySerial, raw->bodySerial);
	return info;
}

ImageInfo readOther(ImageInfo info, const QByteArray& bytes)
{
	QBuffer buffer;
	buffer.setData(bytes);
	buffer.open(QIODevice::ReadOnly);
	QImageReader reader{ &buffer }; // without a format hint, as decoding: by content

	info.format = reader.format();
	const QSize storedSize = reader.size();
	info.uprightSize = reader.transformation().testFlag(QImageIOHandler::TransformationRotate90) ? storedSize.transposed() : storedSize;
	if (info.format == "png" || info.format == "jpeg")
	{
		for (const QString& key : reader.textKeys())
			info.texts.emplace_back(key, reader.text(key));
	}

	EmbeddedMetadata embedded = readEmbeddedMetadata(bytes);
	if (!embedded.iccProfile.isEmpty())
		info.colorProfile = colorProfile(ColorProfile::Source::Embedded, embedded.iccProfile);
	else if (info.format == "jpeg" && readJpegExif(bytes).adobeRgb)
		info.colorProfile = colorProfile(ColorProfile::Source::ExifAdobeRgb, ImageDecoding::adobeRgbIccProfile());
	setExif(info, std::move(embedded.exif), embedded.iccProfile);
	return info;
}

} // namespace

ImageInfo readImageInfo(const QString& path)
{
	const QFileInfo fileInfo{ path };
	ImageInfo info{ .fileSize = fileInfo.size(), .lastModified = fileInfo.lastModified() };
	if (const QString suffix = fileInfo.suffix().toLower(); ImageDecoding::rawFileSuffixes().contains(suffix))
		return readRaw(std::move(info), path, suffix);

	QFile file{ path };
	if (!file.open(QIODevice::ReadOnly))
		return { .error = file.errorString() };
	return readOther(std::move(info), file.readAll());
}

} // namespace ImageMetadata
