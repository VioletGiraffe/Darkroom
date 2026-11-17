#include "ImageMetadata/ImageInfo.h"
#include "ImageDecoding/ImageDecoding.h"
#include "ImageDecoding/LibRawDecoder.h"
#include "ImageMetadata/EmbeddedMetadata.h"
#include "buffered_reader.hpp" // thin_io
#include "qt_paths.hpp"        // thin_io

DISABLE_COMPILER_WARNINGS
#include <QBuffer>
#include <QColorSpace>
#include <QFile>
#include <QFileInfo>
#include <QImageIOHandler>
#include <QImageReader>
RESTORE_COMPILER_WARNINGS

namespace ImageMetadata {

namespace {

std::optional<QString> colorProfileDescription(const QByteArray& iccProfile)
{
	return iccProfile.isEmpty() ? std::nullopt : std::optional{ QColorSpace::fromIccProfile(iccProfile).description() };
}

void setExif(ImageInfo& info, ExifTags exif)
{
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
	info.colorProfile = colorProfileDescription(raw->iccProfile);
	setExif(info, std::move(raw->exif));

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
	info.colorProfile = colorProfileDescription(embedded.iccProfile);
	setExif(info, std::move(embedded.exif));
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
