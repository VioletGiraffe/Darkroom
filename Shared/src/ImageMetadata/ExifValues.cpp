#include "ImageMetadata/ExifValues.h"

DISABLE_COMPILER_WARNINGS
#include <QByteArrayView>
#include <QChar>
#include <QtEndian>
RESTORE_COMPILER_WARNINGS

#include <algorithm>
#include <bit>
#include <optional>

namespace ImageMetadata {

namespace {

constexpr uint16_t ByteType = 1, AsciiType = 2, ShortType = 3, LongType = 4, RationalType = 5, SByteType = 6, UndefinedType = 7,
	SShortType = 8, SLongType = 9, SRationalType = 10, FloatType = 11, DoubleType = 12, IfdType = 13;

constexpr uint16_t XPTitleTag = 0x9C9B, XPSubjectTag = 0x9C9F;
constexpr uint16_t UserCommentTag = 0x9286;
constexpr uint16_t GpsProcessingMethodTag = 0x1B, GpsAreaInformationTag = 0x1C;

template <typename T>
T read(const char* data, bool littleEndian) noexcept
{
	if constexpr (sizeof(T) == 1)
		return static_cast<T>(*data);
	else
		return littleEndian ? qFromLittleEndian<T>(data) : qFromBigEndian<T>(data);
}

// NUL-separated strings, each trimmed, the non-empty ones joined. UTF-8 when valid, else Latin-1, which older writers used.
QString asciiText(QByteArrayView bytes)
{
	QString text;
	while (!bytes.empty())
	{
		const qsizetype nul = bytes.indexOf('\0');
		const QByteArrayView part = nul >= 0 ? bytes.first(nul) : bytes;
		bytes = nul >= 0 ? bytes.sliced(nul + 1) : QByteArrayView{};

		const QString decoded = (part.isValidUtf8() ? QString::fromUtf8(part) : QString::fromLatin1(part)).trimmed();
		if (decoded.isEmpty())
			continue;
		if (!text.isEmpty())
			text += u"; ";
		text += decoded;
	}
	return text;
}

// Up to the first NUL, trimmed
QString utf16Text(QByteArrayView bytes, bool littleEndian)
{
	QString text;
	text.reserve(bytes.size() / 2);
	for (qsizetype i = 0; i + 2 <= bytes.size(); i += 2)
	{
		const auto unit = read<uint16_t>(bytes.data() + i, littleEndian);
		if (unit == 0)
			break;
		text += QChar{ static_cast<char16_t>(unit) };
	}
	return text.trimmed();
}

// 8 bytes naming the character code, then the text; empty for JIS, which is not decoded
std::optional<QString> textWithCharacterCode(QByteArrayView bytes, bool littleEndian)
{
	if (bytes.size() < 8)
		return std::nullopt;

	const QByteArrayView code = bytes.first(8), text = bytes.sliced(8);
	// An undefined code is ASCII in practice
	if (code == QByteArrayView{ "ASCII\0\0\0", 8 } || code == QByteArrayView{ "\0\0\0\0\0\0\0\0", 8 })
		return asciiText(text);
	if (code == QByteArrayView{ "UNICODE\0", 8 })
		return utf16Text(text, littleEndian);
	return std::nullopt;
}

// Empty unless bytes are printable ASCII up to trailing NULs, such as ExifVersion's "0232"
std::optional<QString> printableText(QByteArrayView bytes)
{
	while (!bytes.empty() && bytes.back() == '\0')
		bytes.chop(1);
	if (bytes.empty() || !std::ranges::all_of(bytes, [](char c) { return c >= 0x20 && c <= 0x7E; }))
		return std::nullopt;
	return QString::fromLatin1(bytes);
}

// The values that lie within entry.value: a damaged count does not read past it
qsizetype valueCount(const ExifEntry& entry, qsizetype valueSize) noexcept
{
	return std::min<qsizetype>(entry.count, entry.value.size() / valueSize);
}

template <typename T>
std::vector<int64_t> integers(const ExifEntry& entry, bool littleEndian)
{
	std::vector<int64_t> values(static_cast<size_t>(valueCount(entry, sizeof(T))));
	for (size_t i = 0; i < values.size(); ++i)
		values[i] = read<T>(entry.value.constData() + i * sizeof(T), littleEndian);
	return values;
}

template <typename T>
std::vector<ExifRational> rationals(const ExifEntry& entry, bool littleEndian)
{
	std::vector<ExifRational> values(static_cast<size_t>(valueCount(entry, 2 * sizeof(T))));
	for (size_t i = 0; i < values.size(); ++i)
	{
		const char* const value = entry.value.constData() + i * 2 * sizeof(T);
		values[i] = { read<T>(value, littleEndian), read<T>(value + sizeof(T), littleEndian) };
	}
	return values;
}

template <typename Float, typename Bits>
std::vector<double> floats(const ExifEntry& entry, bool littleEndian)
{
	static_assert(sizeof(Float) == sizeof(Bits));
	std::vector<double> values(static_cast<size_t>(valueCount(entry, sizeof(Float))));
	for (size_t i = 0; i < values.size(); ++i)
		values[i] = std::bit_cast<Float>(read<Bits>(entry.value.constData() + i * sizeof(Float), littleEndian));
	return values;
}

} // namespace

ExifValue exifEntryValue(const ExifEntry& entry, bool littleEndian)
{
	// Windows writes these as UTF-16LE in BYTEs, whatever the byte order
	if (entry.directory == ExifDirectory::Ifd0 && entry.tag >= XPTitleTag && entry.tag <= XPSubjectTag)
		return utf16Text(entry.value, true);

	const bool hasCharacterCode = (entry.directory == ExifDirectory::Exif && entry.tag == UserCommentTag)
		|| (entry.directory == ExifDirectory::Gps && (entry.tag == GpsProcessingMethodTag || entry.tag == GpsAreaInformationTag));
	if (hasCharacterCode)
	{
		if (std::optional<QString> text = textWithCharacterCode(entry.value, littleEndian))
			return *std::move(text);
	}

	switch (entry.type)
	{
	case ByteType: return integers<uint8_t>(entry, littleEndian);
	case AsciiType: return asciiText(entry.value);
	case ShortType: return integers<uint16_t>(entry, littleEndian);
	case LongType:
	case IfdType: return integers<uint32_t>(entry, littleEndian);
	case RationalType: return rationals<uint32_t>(entry, littleEndian);
	case SByteType: return integers<int8_t>(entry, littleEndian);
	case SShortType: return integers<int16_t>(entry, littleEndian);
	case SLongType: return integers<int32_t>(entry, littleEndian);
	case SRationalType: return rationals<int32_t>(entry, littleEndian);
	case FloatType: return floats<float, uint32_t>(entry, littleEndian);
	case DoubleType: return floats<double, uint64_t>(entry, littleEndian);
	case UndefinedType:
		if (std::optional<QString> text = printableText(entry.value))
			return *std::move(text);
		return entry.value;
	default: return entry.value;
	}
}

} // namespace ImageMetadata
