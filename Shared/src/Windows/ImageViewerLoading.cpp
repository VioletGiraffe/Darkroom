#include "Windows/ImageViewerLoading.h"
#include "Core/CpuThreadPool.h"
#include "Core/IoThreadPool.h"

DISABLE_COMPILER_WARNINGS
#include <QColorSpace>
#include <QImage>
RESTORE_COMPILER_WARNINGS

#include <memory>
#include <optional>
#include <utility>

using namespace ImageDecoding;

namespace ImageViewerLoading {

namespace {

// From 1/2 up, the full image is at most twice the box on a side: its own decode follows too soon for a preview to pay
constexpr int MaxPreviewScaleEighths = 3;

// Shared lifetime spans the read, the decodes and the AsyncRequest
struct LoadJob {
	LoadJob(QObject* context, std::function<void(Stage)> done, QString path, RawRendering rawRendering, QSize uprightBox)
		: delivery{ context, std::move(done) }
		, path{ std::move(path) }
		, rawRendering{ rawRendering }
		, uprightBox{ uprightBox }
	{}

	AsyncDelivery<Stage> delivery;
	const QString path;
	const RawRendering rawRendering;
	const QSize uprightBox;
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

void decodeRawPreview(const std::shared_ptr<LoadJob>& job, const EncodedImage& preview, QSize uprightRenderSize)
{
	if (job->delivery.isCancelled())
		return;

	const std::optional<JpegReduction> reduction = planJpegReduction(preview, job->uprightBox);
	deliverPreview(job, reduction ? decodeReduced(preview, reduction->scaleEighths) : decode(preview), uprightRenderSize);
}

void decodeFinal(const std::shared_ptr<LoadJob>& job, const EncodedImage& encoded)
{
	if (job->delivery.isCancelled())
		return;

	// Without a LibRaw rendering, a JPEG's preview is a reduced decode of the same bytes, decoded first in this same task
	if (!encoded.rawRendering)
	{
		const std::optional<JpegReduction> reduction = planJpegReduction(encoded, job->uprightBox);
		if (reduction && reduction->scaleEighths <= MaxPreviewScaleEighths)
		{
			deliverPreview(job, decodeReduced(encoded, reduction->scaleEighths), reduction->uprightSize);
			if (job->delivery.isCancelled())
				return;
		}
	}

	DecodedImage decoded = decode(encoded, [&job](double fraction) {
		job->delivery.deliver(Stage{ .kind = Stage::Kind::Progress, .progress = fraction }, job);
		return !job->delivery.isCancelled();
	});
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

	// Captured through a pointer: a pool task's captures have a fixed capacity
	EncodedImage encoded = readEncodedImage(job->path, job->rawRendering, [&job](EncodedImage preview, QSize uprightRenderSize) {
		cpuThreadPool().enqueue([job, preview = std::make_unique<EncodedImage>(std::move(preview)), uprightRenderSize] {
			decodeRawPreview(job, *preview, uprightRenderSize);
		});
		return !job->delivery.isCancelled();
	});
	if (job->delivery.isCancelled())
		return;

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
