#pragma once

// Builders for JPEG marker segments and the EXIF data they carry

#include <QByteArray>
#include <QtEndian>

#include <cstdint>
#include <vector>

enum class ByteOrder { Little, Big };

inline constexpr uint16_t OrientationTag = 0x0112;
inline constexpr uint16_t MakeTag = 0x010F;

struct IfdEntry
{
	uint16_t tag;
	uint16_t value;
};

template <typename T>
[[nodiscard]] QByteArray bytesOf(T value, ByteOrder order)
{
	QByteArray bytes(sizeof(T), Qt::Uninitialized);
	if (order == ByteOrder::Little)
		qToLittleEndian(value, bytes.data());
	else
		qToBigEndian(value, bytes.data());
	return bytes;
}

// A TIFF header, then IFD0 at offset 8 holding one SHORT per entry
[[nodiscard]] inline QByteArray exifTiff(ByteOrder order, const std::vector<IfdEntry>& entries)
{
	QByteArray tiff = order == ByteOrder::Little ? "II" : "MM";
	tiff += bytesOf<uint16_t>(42, order) + bytesOf<uint32_t>(8, order) + bytesOf(static_cast<uint16_t>(entries.size()), order);
	for (const IfdEntry& entry : entries)
		tiff += bytesOf(entry.tag, order) + bytesOf<uint16_t>(3, order) + bytesOf<uint32_t>(1, order) + bytesOf(entry.value, order) + QByteArray(2, '\0');
	return tiff + bytesOf<uint32_t>(0, order); // no next IFD
}

// FF, the marker code, then a big-endian length that counts itself
[[nodiscard]] inline QByteArray segment(uint8_t marker, const QByteArray& payload)
{
	return QByteArray{ "\xFF" } + static_cast<char>(marker) + bytesOf(static_cast<uint16_t>(payload.size() + 2), ByteOrder::Big) + payload;
}

// IFD0 points to an Exif IFD holding ColorSpace, which points to an interoperability IFD holding interoperabilityIndex if given
[[nodiscard]] inline QByteArray exifTiffWithColorSpace(ByteOrder order, uint16_t colorSpace, const QByteArray& interoperabilityIndex = {})
{
	constexpr uint16_t Ascii = 2, Short = 3, Long = 4;
	const auto entry = [&](uint16_t tag, uint16_t type, const QByteArray& value) {
		return bytesOf(tag, order) + bytesOf(type, order) + bytesOf<uint32_t>(type == Ascii ? 4 : 1, order) + value.leftJustified(4, '\0', true);
	};
	const auto ifd = [&](const std::vector<QByteArray>& entries) {
		QByteArray bytes = bytesOf(static_cast<uint16_t>(entries.size()), order);
		for (const QByteArray& e : entries)
			bytes += e;
		return bytes + bytesOf<uint32_t>(0, order);
	};

	const bool withInteroperability = !interoperabilityIndex.isEmpty();
	const uint32_t exifIfd = 8 + 2 + 12 + 4;
	const uint32_t interoperabilityIfd = exifIfd + 2 + 12 * (withInteroperability ? 2 : 1) + 4;
	std::vector<QByteArray> exifEntries{ entry(0xA001, Short, bytesOf(colorSpace, order)) };
	if (withInteroperability)
		exifEntries.push_back(entry(0xA005, Long, bytesOf(interoperabilityIfd, order)));

	QByteArray tiff = (order == ByteOrder::Little ? "II" : "MM") + bytesOf<uint16_t>(42, order) + bytesOf<uint32_t>(8, order)
		+ ifd({ entry(0x8769, Long, bytesOf(exifIfd, order)) }) + ifd(exifEntries);
	if (withInteroperability)
		tiff += ifd({ entry(0x0001, Ascii, interoperabilityIndex) });
	return tiff;
}

[[nodiscard]] inline QByteArray exifSegment(const QByteArray& tiff)
{
	return segment(0xE1, QByteArray{ "Exif\0\0", 6 } + tiff);
}
