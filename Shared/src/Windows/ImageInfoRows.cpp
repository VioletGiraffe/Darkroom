#include "Windows/ImageInfoRows.h"
#include "ImageMetadata/CaptureMetadata.h"
#include "ImageMetadata/ExifTagNames.h"

DISABLE_COMPILER_WARNINGS
#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QObject>
#include <QStringList>
RESTORE_COMPILER_WARNINGS

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

using namespace ImageMetadata;

namespace ImageInfoRows {

namespace {

constexpr size_t MaxListedValues = 8;
constexpr qsizetype MaxHexBytes = 16;

double toDouble(ExifRational rational) noexcept
{
	return static_cast<double>(rational.numerator) / static_cast<double>(rational.denominator);
}

// At most decimals digits after the point, without trailing zeros
QString decimalText(double value, int decimals)
{
	QString text = QString::number(value, 'f', decimals);
	if (text.contains(u'.'))
	{
		while (text.endsWith(u'0'))
			text.chop(1);
		if (text.endsWith(u'.'))
			text.chop(1);
	}
	return text;
}

QString rationalText(ExifRational rational)
{
	return rational.denominator == 1 ? QString::number(rational.numerator) : QStringLiteral("%1/%2").arg(rational.numerator).arg(rational.denominator);
}

// The name name(code) gives, else the number
QString codeText(uint16_t code, std::string_view (*name)(uint16_t) noexcept)
{
	const std::string_view text = name(code);
	return text.empty() ? QString::number(code) : QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

// first and second joined, or second alone when it starts with first: many models repeat their make
QString joinedNames(const QString& first, const QString& second)
{
	if (first.isEmpty() || second.startsWith(first, Qt::CaseInsensitive))
		return second;
	return second.isEmpty() ? first : first + u' ' + second;
}

QString fileSizeText(int64_t size)
{
	const QLocale locale;
	return QObject::tr("%1 (%2 bytes)").arg(locale.formattedDataSize(size), locale.toString(size));
}

QString captureTimeText(const CaptureTime& captureTime)
{
	QString text = QLocale{}.toString(captureTime.date, QLocale::ShortFormat) + u' '
		+ captureTime.time.toString(captureTime.time.msec() != 0 ? u"HH:mm:ss.zzz" : u"HH:mm:ss");
	if (captureTime.utcOffsetSeconds)
	{
		const int offset = std::abs(*captureTime.utcOffsetSeconds);
		text += QStringLiteral(" UTC%1%2:%3").arg(*captureTime.utcOffsetSeconds < 0 ? u'-' : u'+')
			.arg(offset / 3600, 2, 10, u'0').arg(offset % 3600 / 60, 2, 10, u'0');
	}
	return text;
}

QString coordinatesText(const GpsPosition& gps)
{
	return QObject::tr("%1° %2, %3° %4").arg(decimalText(std::abs(gps.latitude), 6), gps.latitude < 0 ? QObject::tr("S") : QObject::tr("N"),
		decimalText(std::abs(gps.longitude), 6), gps.longitude < 0 ? QObject::tr("W") : QObject::tr("E"));
}

QString focalLengthText(const CaptureMetadata& capture)
{
	if (capture.focalLength && capture.focalLength35mm)
		return QObject::tr("%1 mm (%2 mm in 35 mm format)").arg(decimalText(*capture.focalLength, 2)).arg(*capture.focalLength35mm);
	if (capture.focalLength)
		return QObject::tr("%1 mm").arg(decimalText(*capture.focalLength, 2));
	return QObject::tr("%1 mm in 35 mm format").arg(*capture.focalLength35mm);
}

std::vector<Row> fileRows(const QString& path, const ImageInfo& info)
{
	const QFileInfo file{ path };
	std::vector<Row> rows{
		{ QObject::tr("Name"), file.fileName() },
		{ QObject::tr("Folder"), QDir::toNativeSeparators(file.absolutePath()) },
	};
	if (!info.error.isEmpty())
	{
		rows.push_back({ QObject::tr("Error"), info.error });
		return rows;
	}

	rows.push_back({ QObject::tr("Size"), fileSizeText(info.fileSize) });
	if (info.lastModified.isValid())
		rows.push_back({ QObject::tr("Modified"), QLocale{}.toString(info.lastModified, QLocale::ShortFormat) });
	return rows;
}

std::vector<Row> imageRows(const ImageInfo& info)
{
	const CaptureMetadata& capture = info.capture;
	std::vector<Row> rows;
	if (!info.format.isEmpty())
		rows.push_back({ QObject::tr("Format"), QString::fromLatin1(info.format).toUpper() });
	if (!info.uprightSize.isEmpty())
	{
		const QSize size = info.uprightSize;
		const double megapixels = static_cast<double>(size.width()) * size.height() / 1e6;
		rows.push_back({ QObject::tr("Dimensions"), QObject::tr("%1 × %2 (%3 MP)").arg(size.width()).arg(size.height()).arg(megapixels, 0, 'f', 1) });
	}
	if (capture.orientation)
		rows.push_back({ QObject::tr("Orientation"), codeText(*capture.orientation, orientationName) });
	if (info.colorProfile)
		rows.push_back({ QObject::tr("Color profile"), info.colorProfile->isEmpty() ? QObject::tr("Embedded, unnamed") : *info.colorProfile });
	if (capture.colorSpace)
		rows.push_back({ QObject::tr("Color space"), codeText(*capture.colorSpace, colorSpaceName) });
	if (capture == CaptureMetadata{} && info.otherExif.entries.empty())
		rows.push_back({ QObject::tr("EXIF"), QObject::tr("None") });
	return rows;
}

std::vector<Row> cameraRows(const CaptureMetadata& capture)
{
	std::vector<Row> rows;
	const auto add = [&](QString key, QString value) {
		if (!value.isEmpty())
			rows.push_back({ std::move(key), std::move(value) });
	};
	add(QObject::tr("Camera"), joinedNames(capture.make, capture.model));
	add(QObject::tr("Lens"), joinedNames(capture.lensMake, capture.lens));
	add(QObject::tr("Camera serial number"), capture.bodySerial);
	add(QObject::tr("Lens serial number"), capture.lensSerial);
	return rows;
}

std::vector<Row> exposureRows(const CaptureMetadata& capture)
{
	std::vector<Row> rows;
	if (capture.exposureTime)
		rows.push_back({ QObject::tr("Shutter speed"), exposureTimeText(*capture.exposureTime) });
	if (capture.fNumber)
		rows.push_back({ QObject::tr("Aperture"), fNumberText(*capture.fNumber) });
	if (capture.iso)
		rows.push_back({ QObject::tr("ISO"), QString::number(*capture.iso) });
	if (capture.focalLength || capture.focalLength35mm)
		rows.push_back({ QObject::tr("Focal length"), focalLengthText(capture) });
	if (capture.exposureBias)
		rows.push_back({ QObject::tr("Exposure compensation"), exposureBiasText(*capture.exposureBias) });
	if (capture.exposureProgram)
		rows.push_back({ QObject::tr("Exposure program"), codeText(*capture.exposureProgram, exposureProgramName) });
	if (capture.meteringMode)
		rows.push_back({ QObject::tr("Metering mode"), codeText(*capture.meteringMode, meteringModeName) });
	if (capture.flash)
		rows.push_back({ QObject::tr("Flash"), QString::fromStdString(flashDescription(*capture.flash)) });
	if (capture.whiteBalance)
		rows.push_back({ QObject::tr("White balance"), codeText(*capture.whiteBalance, whiteBalanceName) });
	return rows;
}

std::vector<Row> captureRows(const CaptureMetadata& capture)
{
	std::vector<Row> rows;
	if (capture.captureTime)
		rows.push_back({ QObject::tr("Taken"), captureTimeText(*capture.captureTime) });
	const auto add = [&](QString key, const QString& value) {
		if (!value.isEmpty())
			rows.push_back({ std::move(key), value });
	};
	add(QObject::tr("Description"), capture.description);
	add(QObject::tr("Artist"), capture.artist);
	add(QObject::tr("Copyright"), capture.copyright);
	add(QObject::tr("Software"), capture.software);
	return rows;
}

std::vector<Row> locationRows(const CaptureMetadata& capture)
{
	if (!capture.gps)
		return {};

	std::vector<Row> rows{ { QObject::tr("Coordinates"), coordinatesText(*capture.gps) } };
	if (capture.gps->altitude)
		rows.push_back({ QObject::tr("Altitude"), QObject::tr("%1 m").arg(decimalText(*capture.gps->altitude, 1)) });
	return rows;
}

// The entries of directory, by tag
std::vector<Row> exifRows(const ExifTags& tags, ExifDirectory directory)
{
	std::vector<const ExifEntry*> entries;
	for (const ExifEntry& entry : tags.entries)
	{
		if (entry.directory == directory)
			entries.push_back(&entry);
	}
	std::ranges::stable_sort(entries, {}, &ExifEntry::tag);

	std::vector<Row> rows;
	for (const ExifEntry* entry : entries)
	{
		const std::string_view name = exifTagName(directory, entry->tag);
		const QString number = QStringLiteral("0x") + QStringLiteral("%1").arg(entry->tag, 4, 16, u'0').toUpper();
		rows.push_back({
			.key = name.empty() ? QObject::tr("Tag %1").arg(number) : QString::fromLatin1(name.data(), static_cast<qsizetype>(name.size())),
			.value = exifValueText(exifEntryValue(*entry, tags.littleEndian)),
			.keyToolTip = QObject::tr("Tag %1 (%2)").arg(number).arg(entry->tag),
		});
	}
	return rows;
}

} // namespace

std::vector<Group> groups(const QString& path, const ImageInfo& info)
{
	std::vector<Group> groups;
	const auto add = [&](QString title, std::vector<Row> rows) {
		if (!rows.empty())
			groups.push_back({ std::move(title), std::move(rows) });
	};

	add(QObject::tr("File"), fileRows(path, info));
	if (!info.error.isEmpty())
		return groups;

	const CaptureMetadata& capture = info.capture;
	add(QObject::tr("Image"), imageRows(info));
	add(QObject::tr("Camera"), cameraRows(capture));
	add(QObject::tr("Exposure"), exposureRows(capture));
	add(QObject::tr("Capture"), captureRows(capture));
	add(QObject::tr("Location"), locationRows(capture));

	std::vector<Row> texts;
	for (const auto& [key, value] : info.texts)
		texts.push_back({ key, value });
	add(QObject::tr("Texts"), std::move(texts));

	add(QObject::tr("Other tags: IFD0"), exifRows(info.otherExif, ExifDirectory::Ifd0));
	add(QObject::tr("Other tags: Exif"), exifRows(info.otherExif, ExifDirectory::Exif));
	add(QObject::tr("Other tags: GPS"), exifRows(info.otherExif, ExifDirectory::Gps));
	add(QObject::tr("Other tags: Interoperability"), exifRows(info.otherExif, ExifDirectory::Interoperability));
	add(QObject::tr("Other tags: IFD1"), exifRows(info.otherExif, ExifDirectory::Ifd1));
	return groups;
}

QString exposureTimeText(ExifRational seconds)
{
	// Below a second, as the fraction cameras display when one is near
	const double value = toDouble(seconds);
	if (value > 0 && value < 1)
	{
		const double reciprocal = 1 / value;
		if (const double rounded = std::round(reciprocal); std::abs(reciprocal - rounded) < 0.05 * rounded)
			return QObject::tr("1/%1 s").arg(rounded);
	}
	return QObject::tr("%1 s").arg(QString::number(value, 'g', 4));
}

QString fNumberText(double fNumber)
{
	return QStringLiteral("f/%1").arg(QString::number(fNumber, 'g', 3));
}

QString exposureBiasText(ExifRational ev)
{
	const double value = toDouble(ev);
	return QObject::tr("%1 EV").arg(value > 0 ? u'+' + decimalText(value, 2) : decimalText(value, 2));
}

QString exifValueText(const ExifValue& value)
{
	// The first values, and their count when there are more
	const auto listText = [](const auto& values, const auto& valueText) {
		QStringList texts;
		for (size_t i = 0; i < std::min(values.size(), MaxListedValues); ++i)
			texts.push_back(valueText(values[i]));
		const QString listed = texts.join(QStringLiteral(", "));
		return values.size() > MaxListedValues ? QObject::tr("%1 ... (%n values)", nullptr, static_cast<int>(values.size())).arg(listed) : listed;
	};

	return std::visit([&](const auto& alternative) -> QString {
		using T = std::decay_t<decltype(alternative)>;
		if constexpr (std::is_same_v<T, QString>)
			return alternative;
		else if constexpr (std::is_same_v<T, std::vector<int64_t>>)
			return listText(alternative, [](int64_t integer) { return QString::number(integer); });
		else if constexpr (std::is_same_v<T, std::vector<ExifRational>>)
			return listText(alternative, rationalText);
		else if constexpr (std::is_same_v<T, std::vector<double>>)
			return listText(alternative, [](double number) { return QString::number(number, 'g', 6); });
		else
		{
			static_assert(std::is_same_v<T, QByteArray>);
			return alternative.size() <= MaxHexBytes ? QString::fromLatin1(alternative.toHex(' ').toUpper())
			                                         : QObject::tr("%n bytes", nullptr, static_cast<int>(alternative.size()));
		}
	}, value);
}

} // namespace ImageInfoRows
