#include "UiComponents/ImageFilesSource.h"
#include "Core/CpuThreadPool.h"
#include "Core/IoThreadPool.h"
#include "ImageDecoding/ColorManagement.h"
#include "ImageDecoding/ImageDecoding.h"
#include "Utils.h"
#include "assert/advanced_assert.h"
#include "compiler/compiler_warnings_control.h"
#include "timing/profiler.h"

DISABLE_COMPILER_WARNINGS
#include "resize/qimage_resize.h"

#include <QByteArray>
#include <QImageIOHandler>
#include <QPainter>
#include <QtMath>
RESTORE_COMPILER_WARNINGS

#ifdef PROFILING_ENABLED
DISABLE_COMPILER_WARNINGS
#include <QDebug>
#include <QScopeGuard>
RESTORE_COMPILER_WARNINGS
#endif

#include <memory>
#include <mutex>
#include <utility>
#include <vector>

static constexpr int FOLDER_PREVIEW_GAP = 2; // logical px

#ifdef PROFILING_ENABLED
// One per load job, released when its decode ends or a stage finds the job cancelled.
// A batch lasts while any job holds a token: its wall time is the time to fill a screenful of thumbnails.
class LoadBatchToken
{
public:
	LoadBatchToken()
	{
		Batch& b = batch();
		std::lock_guard lock{ b.mutex };
		if (b.jobsInFlight++ == 0)
		{
			b.jobs = 0;
			b.wall.start();
		}
		++b.jobs;
	}

	~LoadBatchToken()
	{
		Batch& b = batch();
		std::lock_guard lock{ b.mutex };
		if (--b.jobsInFlight != 0)
			return;

		qInfo().nospace() << "Thumbnail batch: " << b.jobs << " jobs in " << b.wall.nsElapsed() / 1e6 << " ms";
		PROFILE_REPORT_COUNTERS();
		PROFILE_RESET_COUNTERS();
	}

	LoadBatchToken(const LoadBatchToken&) = delete;
	LoadBatchToken& operator=(const LoadBatchToken&) = delete;

private:
	struct Batch
	{
		std::mutex mutex;
		int jobsInFlight = 0;
		int jobs = 0;
		CTimeElapsed wall;
	};

	static Batch& batch()
	{
		static Batch instance;
		return instance;
	}
};
#endif

namespace {
	// Shared lifetime spans I/O, decode and the Request.
	// Immutable after posting except the token, the delivery's own state, and the read results, each slot written by its own read.
	struct LoadJob {
		LoadJob(QObject* context, std::function<void(ImageSource::Result)> done, QStringList paths, QSize canvasLogical, qreal dpr, bool colorManaged)
			: _delivery{ context, std::move(done) }
			, _paths{ std::move(paths) }
			, _encodedPerPath(_paths.size())
			, _canvasLogical{ canvasLogical }
			, _dpr{ dpr }
			, _colorManaged{ colorManaged }
		{}

		ImageSource::Delivery _delivery;
		const QStringList _paths;
		std::vector<ImageDecoding::EncodedImage> _encodedPerPath;
		const QSize _canvasLogical;
		const qreal _dpr;
		const bool _colorManaged;
#ifdef PROFILING_ENABLED
		std::unique_ptr<LoadBatchToken> _batchToken;
#endif
	};

	void decodeStage(const std::shared_ptr<LoadJob>& jobPtr, const std::vector<ImageDecoding::EncodedImage>& encodedPerPath)
	{
#ifdef PROFILING_ENABLED
		// Declared before the sample: the batch must not end before the sample is counted.
		const auto releaseBatchToken = qScopeGuard([&] { jobPtr->_batchToken.reset(); });
#endif
		PROFILE_SAMPLE("Thumbnail decode");
		LoadJob& job = *jobPtr;
		if (job._delivery.isCancelled() || encodedPerPath.empty() || job._canvasLogical.isEmpty() || job._dpr <= 0.0)
			return;

		// Laid out in device pixels, so every image lands 1:1 on whole canvas pixels.
		// Floored: the delivered logical size must not exceed the box.
		const int n = static_cast<int>(encodedPerPath.size());
		const int gapPx = qRound(FOLDER_PREVIEW_GAP * job._dpr);
		const int slotWidth = (qFloor(job._canvasLogical.width() * job._dpr) - (n - 1) * gapPx) / n;
		const int slotHeight = qFloor(job._canvasLogical.height() * job._dpr);
		if (slotWidth <= 0 || slotHeight <= 0)
			return;

		struct DecodedFrame {
			QImage fitted;
			QString error;
			QByteArray format;
		};

		// Decoded at full resolution: all scaling goes through ImageProcessing::resize, not the decoder's own (JPEG DCT reduction, QImage::scaled).
		std::vector<DecodedFrame> frames(encodedPerPath.size());
		cpuParallelFor(frames.size(), [&](size_t i) {
			if (job._delivery.isCancelled())
				return;

			DecodedFrame& frame = frames[i];
			ImageDecoding::DecodedImage decoded = [&] {
				PROFILE_SAMPLE("Thumbnail image decode");
				return ImageDecoding::decode(encodedPerPath[i]);
			}();
			frame.format = std::move(decoded.format);
			if (decoded.image.isNull())
			{
				frame.error = std::move(decoded.error);
				return;
			}

			// src is as stored: the orientation and the color conversion are applied after the resize, at thumbnail size
			const QImage& src = decoded.image;
			const QImageIOHandler::Transformations orientation = decoded.orientation;
			const bool rotates90 = orientation.testFlag(QImageIOHandler::TransformationRotate90);

			// At least 1 px: an extreme aspect ratio fits the slot at zero width or height
			const QSize orientedSizePx = (rotates90 ? src.size().transposed() : src.size()).scaled(QSize(slotWidth, slotHeight), Qt::KeepAspectRatio).expandedTo({ 1, 1 });
			const QSize sizePx = rotates90 ? orientedSizePx.transposed() : orientedSizePx;
			frame.fitted = QImage{ sizePx, src.format() };
			{
				PROFILE_SAMPLE("Thumbnail resize");
				if (!ImageProcessing::resize(frame.fitted, src, {}, cpuParallelFor))
					frame.fitted = src.scaled(sizePx, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
			}

			if (job._colorManaged)
				ImageDecoding::convertToSrgb(frame.fitted, decoded.iccProfile);
			ImageDecoding::applyOrientation(frame.fitted, orientation);
		});

		// A frame skipped on cancellation has neither an image nor an error
		if (job._delivery.isCancelled())
			return;

		std::vector<QImage> fitted;
		fitted.reserve(n);
		QString firstError;
		QByteArray firstFormat;
		for (DecodedFrame& frame : frames)
		{
			if (firstFormat.isEmpty())
				firstFormat = frame.format;
			if (frame.fitted.isNull())
			{
				if (firstError.isEmpty())
					firstError = frame.error;
			}
			else
				fitted.push_back(std::move(frame.fitted));
		}

		if (fitted.empty())
		{
			assert_r(!firstError.isEmpty());
			job._delivery.deliver({ .error = firstError, .fileType = firstFormat }, jobPtr);
			return;
		}

		QImage image;
		if (fitted.size() == 1)
		{
			// Blitted on every paint: kept in a format the raster engine blends without per-pixel conversion
			image = std::move(fitted.front());
			if (image.format() != QImage::Format_RGB32 && image.format() != QImage::Format_ARGB32_Premultiplied)
				image.convertTo(QImage::Format_ARGB32_Premultiplied);
		}
		else
		{
			int totalWidth = (static_cast<int>(fitted.size()) - 1) * gapPx;
			int totalHeight = 0;
			for (const QImage& img : std::as_const(fitted))
			{
				totalWidth += img.width();
				totalHeight = qMax(totalHeight, img.height());
			}

			image = QImage{ totalWidth, totalHeight, QImage::Format_ARGB32_Premultiplied };
			image.fill(Qt::transparent);
			QPainter painter(&image); // ends before the pixel ratio is set
			// Frames never overlap on the transparent canvas: Source equals SourceOver and copies instead of blending
			painter.setCompositionMode(QPainter::CompositionMode_Source);
			int x = 0;
			for (const QImage& img : std::as_const(fitted))
			{
				painter.drawImage(QPoint(x, (totalHeight - img.height()) / 2), img);
				x += img.width() + gapPx;
			}
		}
		image.setDevicePixelRatio(job._dpr);

		job._delivery.deliver({ .image = std::move(image), .fileType = firstFormat }, jobPtr);
	}

	void readStage(LoadJob& job, size_t index)
	{
		if (job._delivery.isCancelled())
			return;

		PROFILE_SAMPLE("Thumbnail read");
		job._encodedPerPath[index] = ImageDecoding::readEncodedImage(job._paths[static_cast<qsizetype>(index)], ImageDecoding::RawRendering::EmbeddedPreview);
	}

	// Only called once every read has returned: their samples are counted before the decode can end the batch.
	void postDecode(const std::shared_ptr<LoadJob>& jobPtr)
	{
		LoadJob& job = *jobPtr;
		if (job._delivery.isCancelled())
			return;

		// Moved out of the job: the Request keeps the job alive past delivery
		cpuThreadPool().enqueue([jobPtr, encoded = std::move(job._encodedPerPath)] {
			decodeStage(jobPtr, encoded);
		});
	}

} // anonymous namespace

ImageFilesSource::ImageFilesSource(QStringList paths)
	: _paths{ std::move(paths) }
{
}

std::optional<ImageSource::Result> ImageFilesSource::renderNow(QSize /*box*/, qreal /*dpr*/) const
{
	if (_paths.empty())
		return ImageSource::Result{};
	return std::nullopt;
}

ImageSource::Request ImageFilesSource::renderAsync(QSize box, qreal dpr, QObject* context, std::function<void(ImageSource::Result)> done) const
{
	assert_and_return_r(!_paths.empty(), {});

	auto job = std::make_shared<LoadJob>(context, std::move(done), _paths, box, dpr, colorManagementSetting());
#ifdef PROFILING_ENABLED
	job->_batchToken = std::make_unique<LoadBatchToken>();
#endif
	ImageSource::Request request{ std::shared_ptr<ImageSource::Delivery>{ job, &job->_delivery } };
	// A card's files share one storage device: the first path routes all of them
	IoThreadPool::parallelForAsync(_paths.front(), _paths.size(), [job](size_t index) { readStage(*job, index); }, [job] { postDecode(job); });
	return request;
}
