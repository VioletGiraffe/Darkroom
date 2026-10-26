#include "ImageDecoding/LibRawDecoder.h"
#include "buffered_reader.hpp" // thin_io

DISABLE_COMPILER_WARNINGS
#include <libraw/libraw.h>
RESTORE_COMPILER_WARNINGS

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ImageDecoding {

namespace {

// LibRaw's input, with the stdio semantics LibRaw expects; seeks clamp the position to the file
class BufferedReaderStream final : public LibRaw_abstract_datastream
{
public:
	BufferedReaderStream(thin_io::buffered_reader& file, uint64_t size) noexcept
		: _file{ file }
		, _size{ static_cast<INT64>(size) }
	{
		_file.set_pos(0);
	}

	// Empty unless a read failed: LibRaw reports a failed read as a malformed file
	[[nodiscard]] const QString& ioError() const noexcept { return _ioError; }

	int valid() override { return _file.is_open(); }

	// As fread: whole items
	int read(void* dest, size_t itemSize, size_t itemCount) override
	{
		if (itemSize == 0)
			return 0;

		const std::optional<uint64_t> bytesRead = _file.read(dest, uint64_t{ itemSize } * itemCount);
		if (!bytesRead)
		{
			recordIoError();
			return 0;
		}
		return static_cast<int>(*bytesRead / itemSize);
	}

	int seek(INT64 offset, int whence) override
	{
		INT64 origin = 0;
		switch (whence)
		{
		case SEEK_SET: break;
		case SEEK_CUR: origin = tell(); break;
		case SEEK_END: origin = _size; break;
		default: return -1;
		}

		// Clamped without overflow: offsets come from the file's own, possibly corrupt, fields
		const INT64 target = offset < -origin ? 0 : offset > _size - origin ? _size : origin + offset;
		_file.set_pos(static_cast<uint64_t>(target));
		return 0;
	}

	INT64 tell() override { return static_cast<INT64>(_file.pos()); }
	INT64 size() override { return _size; }
	int eof() override { return tell() >= _size; }

	int get_char() override
	{
		unsigned char c = 0;
		return read(&c, 1, 1) == 1 ? c : EOF;
	}

	// As fgets: through the next newline, at most bufferSize - 1 bytes
	char* gets(char* buffer, int bufferSize) override
	{
		const std::span<const std::byte> bytes = peek(static_cast<size_t>(std::max(bufferSize - 1, 0)));
		if (bytes.empty())
			return nullptr;

		const auto newline = std::ranges::find(bytes, std::byte{ '\n' });
		const size_t length = static_cast<size_t>(newline - bytes.begin()) + (newline == bytes.end() ? 0 : 1);
		std::ranges::transform(bytes.first(length), buffer, [](std::byte b) { return static_cast<char>(b); });
		buffer[length] = '\0';
		_file.set_pos(_file.pos() + length);
		return buffer;
	}

	// As fscanf with one conversion: LibRaw's parsers pass %d and %f
	int scanf_one(const char* format, void* value) override
	{
		constexpr size_t maxTextLength = 24; // as LibRaw's own streams
		std::array<char, maxTextLength + 1> text{};
		std::ranges::transform(peek(maxTextLength), text.begin(), [](std::byte b) { return static_cast<char>(b); });

		const std::string formatCountingConsumed = std::string{ format } + "%n";
		int consumed = 0;
#ifdef _MSC_VER
		const int converted = sscanf_s(text.data(), formatCountingConsumed.c_str(), value, &consumed);
#else
		const int converted = std::sscanf(text.data(), formatCountingConsumed.c_str(), value, &consumed);
#endif
		if (converted > 0)
			_file.set_pos(_file.pos() + static_cast<uint64_t>(consumed));
		return converted;
	}

private:
	// Empty at the end of the file or on a failed read
	std::span<const std::byte> peek(size_t size)
	{
		const std::optional<std::span<const std::byte>> bytes = _file.peek(size);
		if (!bytes)
			recordIoError();
		return bytes.value_or(std::span<const std::byte>{});
	}

	void recordIoError()
	{
		if (_ioError.isEmpty())
			_ioError = QString::fromStdString(thin_io::buffered_reader::text_for_last_error());
	}

private:
	thin_io::buffered_reader& _file;
	const INT64 _size;
	QString _ioError;
};

// LibRaw's flip codes, each mapped as the EXIF orientation LibRaw parses it from
QImageIOHandler::Transformations orientationForFlip(int flip) noexcept
{
	switch (flip)
	{
	case 1: return QImageIOHandler::TransformationMirror;
	case 2: return QImageIOHandler::TransformationFlip;
	case 3: return QImageIOHandler::TransformationRotate180;
	case 4: return QImageIOHandler::TransformationFlipAndRotate90;
	case 5: return QImageIOHandler::TransformationRotate270;
	case 6: return QImageIOHandler::TransformationRotate90;
	case 7: return QImageIOHandler::TransformationMirrorAndRotate90;
	default: return QImageIOHandler::TransformationNone;
	}
}

// On LIBRAW_SUCCESS, imgdata.thumbnail holds the largest JPEG preview
int unpackLargestJpegPreview(LibRaw& raw)
{
	const libraw_thumbnail_list_t& previews = raw.imgdata.thumbs_list;
	std::vector<int> jpegPreviews;
	for (int i = 0; i < previews.thumbcount; ++i)
	{
		if (previews.thumblist[i].tformat == LIBRAW_INTERNAL_THUMBNAIL_JPEG)
			jpegPreviews.push_back(i);
	}

	// Ranked by byte length: a preview's dimensions are not always known before it is unpacked.
	// A Canon CR3's preview listed as JPEG may unpack as H.265.
	std::ranges::sort(jpegPreviews, std::greater{}, [&](int i) { return previews.thumblist[i].tlength; });
	for (const int i : jpegPreviews)
	{
		if (raw.unpack_thumb_ex(i) == LIBRAW_SUCCESS && raw.imgdata.thumbnail.tformat == LIBRAW_THUMBNAIL_JPEG)
			return LIBRAW_SUCCESS;
	}
	return previews.thumbcount == 0 ? LIBRAW_NO_THUMBNAIL : LIBRAW_UNSUPPORTED_THUMBNAIL;
}

} // namespace

EncodedImage readRawPreview(thin_io::buffered_reader& file)
{
	const std::optional<uint64_t> size = file.size();
	if (!size)
		return { .error = QString::fromStdString(thin_io::buffered_reader::text_for_last_error()) };

	// Declared first: LibRaw reads from it until destroyed
	BufferedReaderStream stream{ file, *size };
	// Heap-allocated: a LibRaw instance is hundreds of KB
	const auto raw = std::make_unique<LibRaw>();
	int result = raw->open_datastream(&stream);
	if (result == LIBRAW_SUCCESS)
		result = unpackLargestJpegPreview(*raw);
	if (result != LIBRAW_SUCCESS)
		return { .error = stream.ioError().isEmpty() ? QString::fromLatin1(LibRaw::strerror(result)) : stream.ioError() };

	const libraw_thumbnail_t& preview = raw->imgdata.thumbnail;
	return {
		.bytes = QByteArray{ preview.thumb, static_cast<qsizetype>(preview.tlength) },
		.orientation = orientationForFlip(raw->imgdata.sizes.flip),
	};
}

} // namespace ImageDecoding
