#include "Windows/ImageViewerLoading.h"
#include "Core/CpuThreadPool.h"
#include "Core/IoThreadPool.h"
#include "timing/ctimeelapsed.h"

DISABLE_COMPILER_WARNINGS
#include <QColorSpace>
#include <QImage>
RESTORE_COMPILER_WARNINGS

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>

using namespace ImageDecoding;

namespace ImageViewerLoading {

namespace {

// A JPEG whose full decode is predicted faster gets no preview: its load already feels instant
constexpr double MinJpegDecodeMsForPreview = 100.0;
// A JPEG's preview shows only until its full decode, running in parallel, replaces it: the fastest scale
constexpr int JpegPreviewScaleEighths = 1;

// This machine's full JPEG decodes, a running average; it starts near a Raspberry Pi 4's, so a fast machine sees a few needless previews
std::atomic<double> jpegDecodeMsPerMegapixel{ 15.0 };

double megapixels(QSize size)
{
	return static_cast<double>(size.width()) * size.height() / 1e6;
}

// Concurrent decodes may lose one another's update: harmless for an average
void recordJpegDecode(uint64_t elapsedUs, QSize uprightSize)
{
	const double sample = static_cast<double>(elapsedUs) / 1000.0 / megapixels(uprightSize);
	const double average = jpegDecodeMsPerMegapixel.load(std::memory_order_relaxed);
	jpegDecodeMsPerMegapixel.store(average + (sample - average) / 4, std::memory_order_relaxed);
}

// Shared lifetime spans the read, the decodes and the AsyncRequest
struct LoadJob {
	LoadJob(QObject* context, std::function<void(Stage)> done, QString path, RawRendering rawRendering, QSize uprightBox)
		: delivery{ context, std::move(done) }
		, path{ std::move(path) }
		, rawRendering{ rawRendering }
		, uprightBox{ uprightBox }
	{}

	// rawPreview decoded once, by whichever task needs it first: the one showing it, or LibRaw's rendering, which matches its exposure to it
	[[nodiscard]] const DecodedImage& decodedRawPreview()
	{
		std::call_once(_rawPreviewDecoded, [this] { _decodedRawPreview = decodeRawPreview(rawPreview, uprightBox); });
		return _decodedRawPreview;
	}

	AsyncDelivery<Stage> delivery;
	const QString path;
	const RawRendering rawRendering;
	const QSize uprightBox;
	EncodedImage rawPreview; // a camera raw's embedded JPEG, empty when the read hands over none; set by readStage before any task reads it

private:
	std::once_flag _rawPreviewDecoded;
	DecodedImage _decodedRawPreview;
};

// Requires a decoded image. Turned upright, as the viewer shows it.
CImageViewerWidget::DecodedFile toDecodedFile(DecodedImage decoded)
{
	applyOrientation(decoded.image, decoded.orientation);
	// Save as: Qt's writers embed the profile from the color space, which Qt parses only from some profiles
	if (!decoded.image.colorSpace().isValid() && !decoded.iccProfile.isEmpty())
		decoded.image.setColorSpace(QColorSpace::fromIccProfile(decoded.iccProfile));
	return CImageViewerWidget::DecodedFile{ std::move(decoded.image), QString::fromLatin1(decoded.format), std::move(decoded.iccProfile) };
}

// A preview that fails to decode is skipped: the final stage reports the file's state
void deliverPreview(const std::shared_ptr<LoadJob>& job, DecodedImage decoded, QSize finalSize)
{
	if (decoded.image.isNull())
		return;

	job->delivery.deliver(Stage{ .kind = Stage::Kind::Preview, .file = toDecodedFile(std::move(decoded)), .finalSize = finalSize }, job);
}

void deliverRawPreview(const std::shared_ptr<LoadJob>& job, QSize uprightRenderSize)
{
	if (!job->delivery.isCancelled())
		deliverPreview(job, job->decodedRawPreview(), uprightRenderSize);
}

void decodeFinal(const std::shared_ptr<LoadJob>& job, const EncodedImage& encoded)
{
	if (job->delivery.isCancelled())
		return;

	// Without a LibRaw rendering, a JPEG's preview is a reduced decode of the same bytes, in a task beside this one
	const QSize jpegSize = readReducibleJpegUprightSize(encoded);
	if (!jpegSize.isEmpty() && jpegDecodeMsPerMegapixel.load(std::memory_order_relaxed) * megapixels(jpegSize) >= MinJpegDecodeMsForPreview)
	{
		cpuThreadPool().enqueue([job, preview = std::make_unique<EncodedImage>(encoded), jpegSize] {
			if (!job->delivery.isCancelled())
				deliverPreview(job, decodeReduced(*preview, JpegPreviewScaleEighths), jpegSize);
		});
	}

	const CTimeElapsed decodeTimer{ true };
	const auto onRawProgress = [&job](double fraction) {
		job->delivery.deliver(Stage{ .kind = Stage::Kind::Progress, .progress = fraction }, job);
		return !job->delivery.isCancelled();
	};
	// Without a preview from the read, decode() looks for one in the whole file
	const DecodedRawPreviewSource decodedRawPreview = job->rawPreview.bytes.isEmpty()
		? DecodedRawPreviewSource{}
		: DecodedRawPreviewSource{ [&job] { return job->decodedRawPreview(); } };
	DecodedImage decoded = decode(encoded, onRawProgress, decodedRawPreview);
	if (!jpegSize.isEmpty() && !decoded.image.isNull())
		recordJpegDecode(decodeTimer.usElapsed(), jpegSize);
	if (decoded.animated)
		job->delivery.deliver(Stage{ .kind = Stage::Kind::Animated }, job);
	else if (decoded.image.isNull())
		job->delivery.deliver(Stage{ .kind = Stage::Kind::Failed, .error = std::move(decoded.error) }, job);
	else
		job->delivery.deliver(Stage{ .kind = Stage::Kind::Final, .file = toDecodedFile(std::move(decoded)) }, job);
}

void readStage(const std::shared_ptr<LoadJob>& job)
{
	if (job->delivery.isCancelled())
		return;

	EncodedImage encoded = readEncodedImage(job->path, job->rawRendering, [&job](EncodedImage preview, QSize uprightRenderSize) {
		job->rawPreview = std::move(preview);
		cpuThreadPool().enqueue([job, uprightRenderSize] { deliverRawPreview(job, uprightRenderSize); });
		return !job->delivery.isCancelled();
	});
	if (job->delivery.isCancelled())
		return;

	// Captured through a pointer: a pool task's captures have a fixed capacity
	cpuThreadPool().enqueue([job, encoded = std::make_unique<EncodedImage>(std::move(encoded))] { decodeFinal(job, *encoded); });
}

} // namespace

AsyncRequest start(const QString& path, RawRendering rawRendering, QSize uprightBox, QObject* context, std::function<void(Stage)> done)
{
	auto job = std::make_shared<LoadJob>(context, std::move(done), path, rawRendering, uprightBox);
	AsyncRequest request{ std::shared_ptr<AsyncDeliveryState>{ job, &job->delivery } };
	IoThreadPool::enqueue(path, [job] { readStage(job); });
	return request;
}

} // namespace ImageViewerLoading
