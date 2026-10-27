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

[[nodiscard]] inline QByteArray exifSegment(const QByteArray& tiff)
{
	return segment(0xE1, QByteArray{ "Exif\0\0", 6 } + tiff);
}
