#include "ImageDecoding/LibRawDecoder.h"
#include "assert/advanced_assert.h"
#include "buffered_reader.hpp" // thin_io

DISABLE_COMPILER_WARNINGS
#include <QObject>
#include <libraw/libraw.h>
RESTORE_COMPILER_WARNINGS

#include <algorithm>
#include <array>
#include <cmath>
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

// dng_levels.baseline_exposure for a raw without the DNG tag; compared exactly, as LibRaw assigns this value
constexpr float LibRawNoBaselineExposure = -999.f;

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

QSize uprightSize(const libraw_image_sizes_t& sizes)
{
	const QSize stored{ sizes.width, sizes.height };
	return orientationForFlip(sizes.flip).testFlag(QImageIOHandler::TransformationRotate90) ? stored.transposed() : stored;
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

// The share of a rendering's time that one stage LibRaw reports takes
struct StageWeight {
	LibRaw_progress stage;
	double weight;
};

// In rendering order; the copy out of LibRaw takes the rest of the rendering.
// Estimates: the camera raw samples test logs each rendering's progress against time.
constexpr std::array HalfSizeStageWeights{
	StageWeight{ LIBRAW_PROGRESS_LOAD_RAW, 0.6 },
	StageWeight{ LIBRAW_PROGRESS_SCALE_COLORS, 0.1 },
	StageWeight{ LIBRAW_PROGRESS_PRE_INTERPOLATE, 0.05 },
	StageWeight{ LIBRAW_PROGRESS_CONVERT_RGB, 0.15 },
};
constexpr std::array FullStageWeights{
	StageWeight{ LIBRAW_PROGRESS_LOAD_RAW, 0.35 },
	StageWeight{ LIBRAW_PROGRESS_SCALE_COLORS, 0.05 },
	StageWeight{ LIBRAW_PROGRESS_PRE_INTERPOLATE, 0.05 },
	StageWeight{ LIBRAW_PROGRESS_INTERPOLATE, 0.35 },
	StageWeight{ LIBRAW_PROGRESS_CONVERT_RGB, 0.1 },
};

// LibRaw's progress reports, mapped through the stage weights to the whole rendering's progress; onProgress may be empty
class RenderProgress
{
public:
	RenderProgress(LibRaw& raw, RawRendering rawRendering, const RawRenderProgressHandler& onProgress) noexcept
		: _stageWeights{ rawRendering == RawRendering::HalfSize ? std::span<const StageWeight>{ HalfSizeStageWeights } : std::span<const StageWeight>{ FullStageWeights } }
		, _onProgress{ onProgress }
	{
		raw.set_progress_handler(&RenderProgress::onLibRawProgress, this);
	}

	// False when the rendering is abandoned
	[[nodiscard]] bool reportCopyStarted()
	{
		double copyStart = 0.0;
		for (const StageWeight& stageWeight : _stageWeights)
			copyStart += stageWeight.weight;
		return report(copyStart);
	}

private:
	// LibRaw's progress_callback: non-zero cancels.
	// Runs on the thread that called into LibRaw: an OpenMP loop reports only from its master thread, which is that thread.
	static int onLibRawProgress(void* data, LibRaw_progress stage, int iteration, int expected)
	{
		auto& progress = *static_cast<RenderProgress*>(data);
		double stageStart = 0.0;
		for (const StageWeight& stageWeight : progress._stageWeights)
		{
			if (stageWeight.stage == stage)
			{
				// Iterations run from 0 to expected - 1, the last one at the stage's end
				const double stageDone = std::clamp(static_cast<double>(iteration) / std::max(expected - 1, 1), 0.0, 1.0);
				return progress.report(stageStart + stageWeight.weight * stageDone) ? 0 : 1;
			}
			stageStart += stageWeight.weight;
		}
		// An unweighted stage still lets the handler abandon the rendering
		return progress.report(progress._reported) ? 0 : 1;
	}

	bool report(double fraction)
	{
		_reported = std::max(_reported, fraction);
		return !_onProgress || _onProgress(_reported);
	}

private:
	const std::span<const StageWeight> _stageWeights;
	const RawRenderProgressHandler& _onProgress;
	double _reported = 0.0; // never decreases
};

// Maps LibRaw's linear output to sRGB codes through the tone curve of cameras' own JPEGs, fitted to the camera raw samples' embedded JPEGs.
// Each channel goes through the curve, as in cameras: that also matches their saturation.
// exposureGain scales the input; raw white stays white.
std::array<uint8_t, 65536> toneCurveToSrgb(double exposureGain)
{
	// A sigmoid: lifts midtones by about 1.2 EV and compresses the highlights
	// toeOffset keeps the deepest shadows linear
	constexpr double exponent = 1.75, halfInput = 0.222, toeOffset = 0.005;
	const double halfPowered = std::pow(halfInput, exponent);
	const auto sigmoid = [&](double v) {
		const double powered = std::pow(v, exponent);
		return powered / (powered + halfPowered);
	};
	const double black = sigmoid(toeOffset), white = sigmoid(exposureGain + toeOffset);

	std::array<uint8_t, 65536> codes{};
	for (size_t i = 0; i < codes.size(); ++i)
	{
		const double linear = (sigmoid(static_cast<double>(i) / 65535 * exposureGain + toeOffset) - black) / (white - black);
		const double encoded = linear <= 0.0031308 ? 12.92 * linear : 1.055 * std::pow(linear, 1 / 2.4) - 0.055;
		codes[i] = static_cast<uint8_t>(std::lround(encoded * 255));
	}
	return codes;
}

} // namespace

std::optional<EncodedImage> readRawPreview(thin_io::buffered_reader& file, RawRendering rawRendering, QSize* uprightRenderSize)
{
	assert_r(!uprightRenderSize || rawRendering != RawRendering::EmbeddedPreview);

	const std::optional<uint64_t> size = file.size();
	if (!size)
		return EncodedImage{ .error = QString::fromStdString(thin_io::buffered_reader::text_for_last_error()) };

	// Declared first: LibRaw reads from it until destroyed
	BufferedReaderStream stream{ file, *size };
	// Heap-allocated: a LibRaw instance is hundreds of KB
	const auto raw = std::make_unique<LibRaw>();
	int result = raw->open_datastream(&stream);
	if (result == LIBRAW_SUCCESS)
	{
		if (uprightRenderSize)
		{
			const QSize full = uprightSize(raw->imgdata.sizes);
			// LibRaw's half-size rendering merges color filter array blocks, rounding each side up; a raw without one renders whole
			*uprightRenderSize = rawRendering == RawRendering::HalfSize && raw->imgdata.idata.filters != 0
				? QSize{ (full.width() + 1) / 2, (full.height() + 1) / 2 }
				: full;
		}
		result = unpackLargestJpegPreview(*raw);
	}
	if (!stream.ioError().isEmpty())
		return EncodedImage{ .error = stream.ioError() };
	if (result == LIBRAW_NO_THUMBNAIL || result == LIBRAW_UNSUPPORTED_THUMBNAIL)
		return std::nullopt;
	if (result != LIBRAW_SUCCESS)
		return EncodedImage{ .error = QString::fromLatin1(LibRaw::strerror(result)) };

	const libraw_thumbnail_t& preview = raw->imgdata.thumbnail;
	return EncodedImage{
		.bytes = QByteArray{ preview.thumb, static_cast<qsizetype>(preview.tlength) },
		.orientation = orientationForFlip(raw->imgdata.sizes.flip),
		.adobeRgb = raw->imgdata.color.ExifColorSpace == LIBRAW_COLORSPACE_AdobeRGB,
	};
}

DecodedImage decodeRaw(const QByteArray& file, RawRendering rawRendering, const RawRenderProgressHandler& onProgress)
{
	assert_r(rawRendering != RawRendering::EmbeddedPreview);

	const auto raw = std::make_unique<LibRaw>();
	RenderProgress progress{ *raw, rawRendering, onProgress };
	libraw_output_params_t& params = raw->imgdata.params;
	params.use_camera_wb = 1;
	params.half_size = rawRendering == RawRendering::HalfSize;
	params.user_flip = 0; // the orientation is reported, as for previews

	int result = raw->open_buffer(file.constData(), static_cast<size_t>(file.size()));
	// Read before processing: user_flip replaces it
	const int flip = raw->imgdata.sizes.flip;
	if (result == LIBRAW_SUCCESS)
		result = raw->unpack();
	if (result == LIBRAW_SUCCESS)
		result = raw->dcraw_process();
	if (result == LIBRAW_SUCCESS && !progress.reportCopyStarted())
		result = LIBRAW_CANCELLED_BY_CALLBACK;
	if (result != LIBRAW_SUCCESS)
		return { .error = QString::fromLatin1(LibRaw::strerror(result)) };

	// After processing, imgdata.image holds width x height linear pixels in sRGB primaries, as stored: user_flip is 0
	const int width = raw->imgdata.sizes.width, height = raw->imgdata.sizes.height, colors = raw->imgdata.idata.colors;
	if (colors != 1 && colors != 3)
		return { .error = QObject::tr("Unsupported raw image layout: %1 colors").arg(colors) };

	const QString outOfMemory = QObject::tr("Not enough memory to decode a %1x%2 image").arg(width).arg(height);
	QImage image{ width, height, colors == 1 ? QImage::Format_Grayscale8 : QImage::Format_RGB888 };
	if (image.isNull())
		return { .error = outOfMemory };

	// A DNG's BaselineExposure belongs to its intended rendering; LibRaw parses it, applying it is the caller's to do
	const float baselineEv = raw->imgdata.color.dng_levels.baseline_exposure;
	const std::array<uint8_t, 65536> toSrgb = toneCurveToSrgb(baselineEv == LibRawNoBaselineExposure ? 1.0 : std::exp2(baselineEv));
	for (int y = 0; y < height; ++y)
	{
		uchar* code = image.scanLine(y);
		for (const auto& pixel : std::span{ raw->imgdata.image + static_cast<size_t>(y) * width, static_cast<size_t>(width) })
		{
			for (int c = 0; c < colors; ++c)
				*code++ = toSrgb[pixel[c]];
		}
	}

	raw->recycle(); // frees LibRaw's buffers before the conversion allocates
	// Format_RGB32, as decodeJpeg() delivers
	if (colors == 3)
		image.convertTo(QImage::Format_RGB32);
	if (image.isNull())
		return { .error = outOfMemory };

	return { .image = std::move(image), .orientation = orientationForFlip(flip) };
}

QSize readRawUprightSize(thin_io::buffered_reader& file)
{
	const std::optional<uint64_t> size = file.size();
	if (!size)
		return {};

	BufferedReaderStream stream{ file, *size };
	const auto raw = std::make_unique<LibRaw>();
	if (raw->open_datastream(&stream) != LIBRAW_SUCCESS)
		return {};

	return uprightSize(raw->imgdata.sizes);
}

} // namespace ImageDecoding
