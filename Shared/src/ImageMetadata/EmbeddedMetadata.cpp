#include "ImageMetadata/EmbeddedMetadata.h"

DISABLE_COMPILER_WARNINGS
#include <QtEndian>
RESTORE_COMPILER_WARNINGS

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <map>
#include <ranges>

namespace ImageMetadata {

namespace {

constexpr QByteArrayView ExifHeader{ "Exif\0\0", 6 };
constexpr QByteArrayView PngSignature{ "\x89PNG\r\n\x1A\n", 8 };
constexpr uint16_t InterColorProfileTag = 0x8773;

// A PNG's eXIf or a WebP's EXIF chunk: a TIFF structure, after APP1's header when the writer added one
ExifTags exifTagsFromChunk(QByteArrayView chunk)
{
	return chunk.startsWith(ExifHeader) ? exifTagsFromApp1(chunk) : exifTagsFromTiff(chunk);
}

EmbeddedMetadata readJpeg(QByteArrayView jpeg)
{
	// The profile is split across APP2 segments, each holding the header, its 1-based sequence number, the segment count, then its part
	constexpr QByteArrayView IccHeader{ "ICC_PROFILE\0", 12 };
	EmbeddedMetadata metadata;
	std::map<int, QByteArrayView> iccParts;
	int iccPartCount = 0;
	forEachJpegSegment(jpeg, [&](uchar marker, QByteArrayView payload) {
		if (marker == 0xE1 && metadata.exif.entries.empty()) // APP1
			metadata.exif = exifTagsFromApp1(payload);
		else if (marker == 0xE2 && payload.size() > IccHeader.size() + 2 && payload.startsWith(IccHeader))
		{
			iccPartCount = static_cast<uchar>(payload[IccHeader.size() + 1]);
			iccParts.try_emplace(static_cast<uchar>(payload[IccHeader.size()]), payload.sliced(IccHeader.size() + 2));
		}
		return true;
	});

	// A profile missing a part is left out
	if (iccPartCount > 0 && std::ssize(iccParts) == iccPartCount && iccParts.begin()->first == 1 && iccParts.rbegin()->first == iccPartCount)
	{
		for (const QByteArrayView part : iccParts | std::views::values)
			metadata.iccProfile.append(part);
	}
	return metadata;
}

EmbeddedMetadata readTiff(QByteArrayView tiff)
{
	EmbeddedMetadata metadata{ .exif = exifTagsFromTiff(tiff) };
	const auto profile = std::ranges::find_if(metadata.exif.entries, [](const ExifEntry& entry) {
		return entry.directory == ExifDirectory::Ifd0 && entry.tag == InterColorProfileTag;
	});
	if (profile != metadata.exif.entries.end())
		metadata.iccProfile = profile->value;
	return metadata;
}

// An iCCP chunk's: a keyword, NUL, the compression method, then the profile compressed with zlib
QByteArray iccProfileFromIccp(QByteArrayView chunk)
{
	const qsizetype keywordEnd = chunk.indexOf('\0');
	if (keywordEnd < 1 || keywordEnd + 2 > chunk.size() || chunk[keywordEnd + 1] != 0) // 0: zlib
		return {};

	// qUncompress takes the uncompressed size first, big-endian, as a hint only
	QByteArray compressed(4, '\0');
	compressed.append(chunk.sliced(keywordEnd + 2));
	return qUncompress(compressed);
}

EmbeddedMetadata readPng(QByteArrayView png)
{
	// After the signature, each chunk is its data's big-endian length, its type, the data, then a CRC
	EmbeddedMetadata metadata;
	for (qsizetype pos = PngSignature.size(); pos + 12 <= png.size();)
	{
		const qsizetype length = qFromBigEndian<uint32_t>(png.data() + pos);
		if (length > png.size() - pos - 12)
			break;

		const QByteArrayView type = png.sliced(pos + 4, 4), data = png.sliced(pos + 8, length);
		if (type == "eXIf")
			metadata.exif = exifTagsFromChunk(data);
		else if (type == "iCCP")
			metadata.iccProfile = iccProfileFromIccp(data);
		else if (type == "IEND")
			break;

		pos += 12 + length;
	}
	return metadata;
}

EmbeddedMetadata readWebP(QByteArrayView webp)
{
	// After the RIFF header, each chunk is its type, its data's little-endian size, then the data, padded to an even size
	EmbeddedMetadata metadata;
	for (qsizetype pos = 12; pos + 8 <= webp.size();)
	{
		const qsizetype size = qFromLittleEndian<uint32_t>(webp.data() + pos + 4);
		if (size > webp.size() - pos - 8)
			break;

		const QByteArrayView type = webp.sliced(pos, 4), data = webp.sliced(pos + 8, size);
		if (type == "EXIF")
			metadata.exif = exifTagsFromChunk(data);
		else if (type == "ICCP")
			metadata.iccProfile = data.toByteArray();

		pos += 8 + size + size % 2;
	}
	return metadata;
}

} // namespace

EmbeddedMetadata readEmbeddedMetadata(QByteArrayView file)
{
	if (file.startsWith("\xFF\xD8")) // SOI
		return readJpeg(file);
	if (file.startsWith("II") || file.startsWith("MM"))
		return readTiff(file);
	if (file.startsWith(PngSignature))
		return readPng(file);
	if (file.size() >= 12 && file.startsWith("RIFF") && file.sliced(8, 4) == "WEBP")
		return readWebP(file);
	return {};
}

} // namespace ImageMetadata
