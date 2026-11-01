#include "ImageDecoding/LibRawDecoder.h"
#include "ImageDecoding/ColorManagement.h"
#include "ImageDecoding/TurboJpegDecoder.h"
#include "assert/advanced_assert.h"
#include "buffered_reader.hpp" // thin_io

DISABLE_COMPILER_WARNINGS
#include <QImage>
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
		: _raw{ raw }
		, _stageWeights{ rawRendering == RawRendering::HalfSize ? std::span<const StageWeight>{ HalfSizeStageWeights } : std::span<const StageWeight>{ FullStageWeights } }
		, _onProgress{ onProgress }
	{
		raw.set_progress_handler(&RenderProgress::onLibRawProgress, this);
	}

	// The LibRaw instance may outlive this
	~RenderProgress() { _raw.set_progress_handler(nullptr, nullptr); }

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
	LibRaw& _raw;
	const std::span<const StageWeight> _stageWeights;
	const RawRenderProgressHandler& _onProgress;
	double _reported = 0.0; // never decreases
};

// The exposure the camera adds to the raw in its own rendering, in EV; LibRaw parses each part, applying them is the caller's to do
double cameraRenderingExposureEv(const LibRaw& raw)
{
	double ev = 0.0;
	if (const float baselineEv = raw.imgdata.color.dng_levels.baseline_exposure; baselineEv != LibRawNoBaselineExposure)
		ev += baselineEv;

	// Fujifilm's DR200 and DR400 expose the raw 1 and 2 EV under; DynamicRangeSetting 0 is automatic, 1 manual
	const libraw_fuji_info_t& fuji = raw.imgdata.makernotes.fuji;
	const int dynamicRangePercent = fuji.DynamicRangeSetting == 0 ? fuji.AutoDynamicRange : fuji.DynamicRangeSetting == 1 ? fuji.DevelopmentDynamicRange : 0;
	if (dynamicRangePercent > 100 && dynamicRangePercent <= 400)
		ev += std::log2(dynamicRangePercent / 100.0);

	// Canon's Highlight Tone Priority exposes the raw under by its level in EV, as LibRaw's ExposureCalibrationShift counts it
	ev += raw.imgdata.makernotes.canon.HighlightTonePriority;
	return ev;
}

// The tone curve of cameras' own JPEGs, fitted to the camera raw samples' embedded JPEGs.
// Maps LibRaw's linear output, 1 at raw white, to linear light, 0 to 1.
// Each channel goes through the curve, as in cameras: that also matches their saturation.
// exposureGain scales the input; raw white stays white.
class ToneCurve
{
public:
	explicit ToneCurve(double exposureGain) noexcept
		: _exposureGain{ exposureGain }
		, _black{ sigmoid(toeOffset) }
		, _white{ sigmoid(exposureGain + toeOffset) }
	{}

	[[nodiscard]] double operator()(double linear) const noexcept
	{
		const double toned = withShoulder((sigmoid(linear * _exposureGain + toeOffset) - _black) / (_white - _black));
		return std::max(0.0, (toned - blackOffset) / (1 - blackOffset));
	}

private:
	// A sigmoid: lifts midtones by about 1.4 EV and compresses the highlights
	// toeOffset keeps the deepest shadows linear
	// shoulder deepens the contrast above output shoulderStart
	// blackOffset darkens the deepest shadows below the cameras' median: a choice of look, which the fit leaves out
	// scripts/fit_tone_curve.py fits the first four constants and reads all five from this line
	static constexpr double exponent = 1.8221, halfInput = 0.2058, toeOffset = 0.00475, shoulder = 1.3245, blackOffset = 0.0005;
	static constexpr double shoulderStart = 0.4;
	static_assert(shoulder < 3, "withShoulder must stay monotonic");

	static double sigmoid(double v) noexcept
	{
		static const double halfPowered = std::pow(halfInput, exponent);
		const double powered = std::pow(v, exponent);
		return powered / (powered + halfPowered);
	}

	// s(t) = t - shoulder * t^2 * (1 - t), for t from 0 at shoulderStart to 1 at white: slope 1 at shoulderStart, white stays white
	static double withShoulder(double toned) noexcept
	{
		if (toned <= shoulderStart)
			return toned;

		const double t = (toned - shoulderStart) / (1 - shoulderStart);
		return shoulderStart + (1 - shoulderStart) * (t - shoulder * t * t * (1 - t));
	}

private:
	const double _exposureGain;
	const double _black;
	const double _white;
};

double srgbEncoded(double linear)
{
	return linear <= 0.0031308 ? 12.92 * linear : 1.055 * std::pow(linear, 1 / 2.4) - 0.055;
}

using SrgbCodeTable = std::array<uint8_t, 65536>; // indexed by LibRaw's 16-bit linear values

SrgbCodeTable toneCurveToSrgb(double exposureGain)
{
	const ToneCurve toneCurve{ exposureGain };
	SrgbCodeTable codes{};
	for (size_t i = 0; i < codes.size(); ++i)
		codes[i] = static_cast<uint8_t>(std::lround(srgbEncoded(toneCurve(static_cast<double>(i) / 65535)) * 255));
	return codes;
}

// Linear RGB averaged over square cells, a grid MatchingGridWidth cells wide; a partial cell at the right or bottom edge is dropped
using CellColors = std::vector<std::array<float, 3>>;
constexpr int MatchingGridWidth = 160;

// imgdata.image after processing, 1 at raw white; up to 4 x 4 pixels sampled per cell
CellColors renderingCellColors(const LibRaw& raw)
{
	const int width = raw.imgdata.sizes.width, height = raw.imgdata.sizes.height, colors = raw.imgdata.idata.colors;
	const int cellSize = std::max(1, width / MatchingGridWidth), step = std::max(1, cellSize / 4);
	CellColors cells;
	for (int top = 0; top + cellSize <= height; top += cellSize)
	{
		for (int left = 0; left + cellSize <= width; left += cellSize)
		{
			std::array<float, 3> sum{};
			int count = 0;
			for (int y = top; y < top + cellSize; y += step)
			{
				for (int x = left; x < left + cellSize; x += step, ++count)
				{
					const auto& pixel = raw.imgdata.image[static_cast<size_t>(y) * width + x];
					for (int c = 0; c < 3; ++c)
						sum[c] += pixel[colors == 1 ? 0 : c];
				}
			}
			cells.push_back({ sum[0] / (count * 65535.f), sum[1] / (count * 65535.f), sum[2] / (count * 65535.f) });
		}
	}
	return cells;
}

// The largest embedded JPEG preview in sRGB; empty when the raw has none or it does not decode
CellColors previewCellColors(LibRaw& raw)
{
	if (unpackLargestJpegPreview(raw) != LIBRAW_SUCCESS)
		return {};

	const libraw_thumbnail_t& thumbnail = raw.imgdata.thumbnail;
	const QByteArray jpeg = QByteArray::fromRawData(thumbnail.thumb, static_cast<qsizetype>(thumbnail.tlength));
	// Half size, no smaller: a DCT-reduced decode averages sRGB codes, which darkens fine texture.
	// At half size that shifts the match by under 0.1 EV on the camera raw samples, at 1/8 by up to 0.36.
	std::optional<DecodedImage> decoded = decodeJpeg(jpeg, 4);
	if (!decoded || decoded->image.isNull())
		return {};

	QImage& image = decoded->image;
	// As decode() does for a raw's preview: an embedded profile, else Adobe RGB when the EXIF declares it
	const bool exifAdobeRgb = raw.imgdata.color.ExifColorSpace == LIBRAW_COLORSPACE_AdobeRGB;
	convertToSrgb(image, decoded->iccProfile.isEmpty() && exifAdobeRgb ? adobeRgbIccProfile() : decoded->iccProfile);
	image.convertTo(QImage::Format_RGB32);

	static const std::array<float, 256> linearForCode = [] {
		std::array<float, 256> linear{};
		for (size_t code = 0; code < linear.size(); ++code)
		{
			const double encoded = static_cast<double>(code) / 255;
			linear[code] = static_cast<float>(encoded <= 0.04045 ? encoded / 12.92 : std::pow((encoded + 0.055) / 1.055, 2.4));
		}
		return linear;
	}();

	const int cellSize = std::max(1, image.width() / MatchingGridWidth);
	CellColors cells;
	for (int top = 0; top + cellSize <= image.height(); top += cellSize)
	{
		for (int left = 0; left + cellSize <= image.width(); left += cellSize)
		{
			std::array<float, 3> sum{};
			for (int y = top; y < top + cellSize; ++y)
			{
				const auto* line = reinterpret_cast<const QRgb*>(image.constScanLine(y));
				for (int x = left; x < left + cellSize; ++x)
				{
					sum[0] += linearForCode[qRed(line[x])];
					sum[1] += linearForCode[qGreen(line[x])];
					sum[2] += linearForCode[qBlue(line[x])];
				}
			}
			const auto count = static_cast<float>(cellSize * cellSize);
			cells.push_back({ sum[0] / count, sum[1] / count, sum[2] / count });
		}
	}
	return cells;
}

double luminance(const std::array<double, 3>& rgb)
{
	return 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2];
}

// As numpy.percentile: interpolates between the values at the two nearest ranks.
// values must not be empty, and must hold the values of sorted order at those two ranks.
double quantile(const std::vector<double>& values, double fraction)
{
	const double position = fraction * static_cast<double>(values.size() - 1);
	const auto below = static_cast<size_t>(position);
	const size_t above = std::min(below + 1, values.size() - 1);
	return values[below] + (values[above] - values[below]) * (position - static_cast<double>(below));
}

// The exposure shift, in EV within +-2, that matches the rendering's midtones, through ToneCurve at exposureGain, to the preview's.
// Matches log2 luminance quantiles in least squares, over the range of quantiles where the preview is midtone, 0.05 to 0.4 linear:
// that range ignores how differently the camera renders deep shadows and highlights.
// Over quantiles 0.05 to 0.95 when that range holds under 5% of the preview.
double quantileMatchingShiftEv(const CellColors& rendering, const CellColors& preview, double exposureGain)
{
	std::vector<double> previewLuminances;
	previewLuminances.reserve(preview.size());
	for (const auto& [r, g, b] : preview)
		previewLuminances.push_back(luminance({ r, g, b }));
	std::ranges::sort(previewLuminances);

	const auto fractionBelow = [&](double value) {
		return static_cast<double>(std::ranges::lower_bound(previewLuminances, value) - previewLuminances.begin()) / static_cast<double>(previewLuminances.size());
	};
	const double midtonesStart = fractionBelow(0.05), midtonesEnd = fractionBelow(0.4);
	std::vector<double> fractions;
	if (midtonesEnd - midtonesStart >= 0.05)
	{
		for (int i = 0; i <= 10; ++i)
			fractions.push_back(midtonesStart + (midtonesEnd - midtonesStart) * i / 10);
	}
	else
	{
		for (int percent = 5; percent <= 95; percent += 5)
			fractions.push_back(percent / 100.0);
	}

	// Quantiles are clamped to at least half an 8-bit sRGB code: the preview holds nothing darker but 0
	constexpr double darkestLuminance = 0.5 / 255 / 12.92;
	std::vector<double> targets;
	for (const double fraction : fractions)
		targets.push_back(std::log2(std::max(quantile(previewLuminances, fraction), darkestLuminance)));

	// The ranks quantile() reads from renderingLuminances, ascending: nth_element places just these
	std::vector<size_t> ranks;
	for (const double fraction : fractions)
	{
		const auto below = static_cast<size_t>(fraction * static_cast<double>(rendering.size() - 1));
		ranks.push_back(below);
		ranks.push_back(std::min(below + 1, rendering.size() - 1));
	}
	std::ranges::sort(ranks);
	ranks.erase(std::ranges::unique(ranks).begin(), ranks.end());

	std::vector<double> renderingLuminances(rendering.size());
	// The tone curve tabulated, interpolated linearly: a fraction of the cost of evaluating it per cell
	constexpr int curvePoints = 4096;
	std::array<double, curvePoints + 1> curve{};
	const auto cost = [&](double shiftEv) {
		const ToneCurve toneCurve{ exposureGain * std::exp2(shiftEv) };
		for (int i = 0; i <= curvePoints; ++i)
			curve[i] = toneCurve(static_cast<double>(i) / curvePoints);
		const auto curveAt = [&](float linear) {
			const double position = linear * curvePoints;
			const int i = std::min(static_cast<int>(position), curvePoints - 1);
			return curve[i] + (curve[i + 1] - curve[i]) * (position - i);
		};
		std::ranges::transform(rendering, renderingLuminances.begin(), [&](const std::array<float, 3>& rgb) {
			return luminance({ curveAt(rgb[0]), curveAt(rgb[1]), curveAt(rgb[2]) });
		});
		auto unplaced = renderingLuminances.begin();
		for (const size_t rank : ranks)
		{
			const auto ranked = renderingLuminances.begin() + static_cast<std::ptrdiff_t>(rank);
			std::nth_element(unplaced, ranked, renderingLuminances.end());
			unplaced = ranked + 1;
		}
		double sum = 0.0;
		for (size_t i = 0; i < fractions.size(); ++i)
		{
			const double difference = std::log2(std::max(quantile(renderingLuminances, fractions[i]), darkestLuminance)) - targets[i];
			sum += difference * difference;
		}
		return sum;
	};

	// A scan, then a golden-section search within a scan step of its best point, which assumes one minimum there
	constexpr double maxShiftEv = 2.0, scanStep = 0.25, tolerance = 0.01;
	double bestShift = -maxShiftEv;
	double bestCost = cost(bestShift);
	for (double shift = -maxShiftEv + scanStep; shift <= maxShiftEv; shift += scanStep)
	{
		if (const double shiftCost = cost(shift); shiftCost < bestCost)
		{
			bestShift = shift;
			bestCost = shiftCost;
		}
	}

	constexpr double golden = 0.6180339887498949;
	double low = std::max(-maxShiftEv, bestShift - scanStep), high = std::min(maxShiftEv, bestShift + scanStep);
	double lower = high - golden * (high - low), upper = low + golden * (high - low);
	double lowerCost = cost(lower), upperCost = cost(upper);
	while (high - low > tolerance)
	{
		if (lowerCost < upperCost)
		{
			high = upper;
			upper = lower;
			upperCost = lowerCost;
			lower = high - golden * (high - low);
			lowerCost = cost(lower);
		}
		else
		{
			low = lower;
			lower = upper;
			lowerCost = upperCost;
			upper = low + golden * (high - low);
			upperCost = cost(upper);
		}
	}
	return (low + high) / 2;
}

// The shift matching raw's rendering to its embedded JPEG preview; 0 without a usable preview
double previewMatchingShiftEv(LibRaw& raw, double exposureGain)
{
	constexpr size_t minCells = 100;
	const CellColors preview = previewCellColors(raw);
	if (preview.size() < minCells)
		return 0.0;

	const CellColors rendering = renderingCellColors(raw);
	return rendering.size() < minCells ? 0.0 : quantileMatchingShiftEv(rendering, preview, exposureGain);
}

QString notEnoughMemoryError(int width, int height)
{
	return QObject::tr("Not enough memory to decode a %1x%2 image").arg(width).arg(height);
}

// A whole raw file processed by LibRaw: imgdata.image holds width x height linear pixels in sRGB primaries, as stored, with 1 or 3 colors
struct ProcessedRaw {
	std::unique_ptr<LibRaw> raw; // null on failure
	int flip = 0;                // the stored orientation: user_flip replaces it during processing
	QString error;
};

ProcessedRaw processRaw(const QByteArray& file, RawRendering rawRendering, const RawRenderProgressHandler& onProgress)
{
	assert_r(rawRendering != RawRendering::EmbeddedPreview);

	auto raw = std::make_unique<LibRaw>();
	RenderProgress progress{ *raw, rawRendering, onProgress };
	libraw_output_params_t& params = raw->imgdata.params;
	params.use_camera_wb = 1;
	params.half_size = rawRendering == RawRendering::HalfSize;
	params.user_flip = 0; // the orientation is reported, as for previews

	int result = raw->open_buffer(file.constData(), static_cast<size_t>(file.size()));
	const int flip = raw->imgdata.sizes.flip;
	if (result == LIBRAW_SUCCESS)
		result = raw->unpack();
	if (result == LIBRAW_SUCCESS)
		result = raw->dcraw_process();
	if (result == LIBRAW_SUCCESS && !progress.reportCopyStarted())
		result = LIBRAW_CANCELLED_BY_CALLBACK;
	if (result != LIBRAW_SUCCESS)
		return { .error = QString::fromLatin1(LibRaw::strerror(result)) };

	if (const int colors = raw->imgdata.idata.colors; colors != 1 && colors != 3)
		return { .error = QObject::tr("Unsupported raw image layout: %1 colors").arg(colors) };

	return { .raw = std::move(raw), .flip = flip };
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
	const ProcessedRaw processed = processRaw(file, rawRendering, onProgress);
	if (!processed.raw)
		return { .error = processed.error };

	LibRaw& raw = *processed.raw;
	// Before the image is allocated: the matching decodes the preview, and frees it on return
	const double cameraExposureEv = cameraRenderingExposureEv(raw);
	const double exposureEv = cameraExposureEv + previewMatchingShiftEv(raw, std::exp2(cameraExposureEv));
	const SrgbCodeTable toSrgb = toneCurveToSrgb(std::exp2(exposureEv));

	const int width = raw.imgdata.sizes.width, height = raw.imgdata.sizes.height, colors = raw.imgdata.idata.colors;
	QImage image{ width, height, colors == 1 ? QImage::Format_Grayscale8 : QImage::Format_RGB888 };
	if (image.isNull())
		return { .error = notEnoughMemoryError(width, height) };

	for (int y = 0; y < height; ++y)
	{
		uchar* code = image.scanLine(y);
		for (const auto& pixel : std::span{ raw.imgdata.image + static_cast<size_t>(y) * width, static_cast<size_t>(width) })
		{
			for (int c = 0; c < colors; ++c)
				*code++ = toSrgb[pixel[c]];
		}
	}

	raw.recycle(); // frees LibRaw's buffers before the conversion allocates
	// Format_RGB32, as decodeJpeg() delivers
	if (colors == 3)
		image.convertTo(QImage::Format_RGB32);
	if (image.isNull())
		return { .error = notEnoughMemoryError(width, height) };

	return { .image = std::move(image), .orientation = orientationForFlip(processed.flip) };
}

LinearRawRendering decodeRawLinear(const QByteArray& file, RawRendering rawRendering)
{
	const ProcessedRaw processed = processRaw(file, rawRendering, {});
	if (!processed.raw)
		return { .error = processed.error };

	LibRaw& raw = *processed.raw;
	const int width = raw.imgdata.sizes.width, height = raw.imgdata.sizes.height, colors = raw.imgdata.idata.colors;
	QImage image{ width, height, colors == 1 ? QImage::Format_Grayscale16 : QImage::Format_RGBX64 };
	if (image.isNull())
		return { .error = notEnoughMemoryError(width, height) };

	for (int y = 0; y < height; ++y)
	{
		auto* value = reinterpret_cast<uint16_t*>(image.scanLine(y));
		for (const auto& pixel : std::span{ raw.imgdata.image + static_cast<size_t>(y) * width, static_cast<size_t>(width) })
		{
			for (int c = 0; c < colors; ++c)
				*value++ = pixel[c];
			if (colors == 3)
				*value++ = 0xFFFF; // Format_RGBX64's unused channel
		}
	}
	const double cameraExposureEv = cameraRenderingExposureEv(raw);
	return { .image = std::move(image), .cameraExposureEv = cameraExposureEv, .previewShiftEv = previewMatchingShiftEv(raw, std::exp2(cameraExposureEv)) };
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
