#include "UiComponents/ImageFilesSource.h"
#include "Core/CpuThreadPool.h"
#include "Core/IoThreadPool.h"
#include "assert/advanced_assert.h"
#include "compiler/compiler_warnings_control.h"
#include "timing/profiler.h"

DISABLE_COMPILER_WARNINGS
#include "resize/qimage_resize.h"

#include <QBuffer>
#include <QByteArray>
#include <QFile>
#include <QImageReader>
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
	// Shared lifetime spans I/O, decode and the Request; immutable after posting except the token and the delivery's own state.
	struct LoadJob {
		LoadJob(QObject* context, std::function<void(ImageSource::Result)> done) : _delivery{ context, std::move(done) } {}

		ImageSource::Delivery _delivery;
		QStringList _paths;
		QSize _canvasLogical;
		qreal _dpr = 1.0;
#ifdef PROFILING_ENABLED
		std::unique_ptr<LoadBatchToken> _batchToken;
#endif
	};

	void decodeStage(const std::shared_ptr<LoadJob>& jobPtr, std::vector<QByteArray>& bytesPerPath, const QString& readError)
	{
#ifdef PROFILING_ENABLED
		// Declared before the sample: the batch must not end before the sample is counted.
		const auto releaseBatchToken = qScopeGuard([&] { jobPtr->_batchToken.reset(); });
#endif
		PROFILE_SAMPLE("Thumbnail decode");
		LoadJob& job = *jobPtr;
		if (job._delivery.isCancelled() || bytesPerPath.empty() || job._canvasLogical.isEmpty() || job._dpr <= 0.0)
			return;

		// Laid out in device pixels, so every image lands 1:1 on whole canvas pixels.
		// Floored: the delivered logical size must not exceed the box.
		const int n = static_cast<int>(bytesPerPath.size());
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

		// Decoded at full resolution: all scaling goes through ImageProcessing::resize, not the reader's own (JPEG DCT reduction, QImage::scaled).
		std::vector<DecodedFrame> frames(bytesPerPath.size());
		cpuParallelFor(frames.size(), [&](size_t i) {
			if (job._delivery.isCancelled())
				return;

			DecodedFrame& frame = frames[i];
			QBuffer buffer(&bytesPerPath[i]);
			buffer.open(QIODevice::ReadOnly);
			QImageReader reader(&buffer);  // A format hint would suppress content detection.
			reader.setAutoTransform(true); // read() returns the image in its displayed orientation
			frame.format = reader.format();

			const QImage src = [&] {
				PROFILE_SAMPLE("Thumbnail image decode");
				return reader.read();
			}();
			if (src.isNull())
			{
				frame.error = reader.errorString();
				return;
			}

			// At least 1 px: an extreme aspect ratio fits the slot at zero width or height
			const QSize sizePx = src.size().scaled(QSize(slotWidth, slotHeight), Qt::KeepAspectRatio).expandedTo({ 1, 1 });
			frame.fitted = QImage{ sizePx, src.format() };
			{
				PROFILE_SAMPLE("Thumbnail resize");
				if (!ImageProcessing::resize(frame.fitted, src, {}, cpuParallelFor))
					frame.fitted = src.scaled(sizePx, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
			}
		});

		// A frame skipped on cancellation has neither an image nor an error
		if (job._delivery.isCancelled())
			return;

		std::vector<QImage> fitted;
		fitted.reserve(n);
		QString firstError = readError;
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

	// The I/O stage touches only the job, then hands bytes to the CPU pool.
	void readStage(const std::shared_ptr<LoadJob>& jobPtr)
	{
		const LoadJob& job = *jobPtr;
		if (job._delivery.isCancelled())
			return;

		std::vector<QByteArray> bytes;
		QString firstReadError;
		bytes.reserve(job._paths.size());
		{
			// Ends before the decode is posted: the batch must not end before the sample is counted.
			PROFILE_SAMPLE("Thumbnail read");
			for (const QString& path : std::as_const(job._paths))
			{
				QFile file(path);
				if (file.open(QIODevice::ReadOnly))
					bytes.push_back(file.readAll());
				else
				{
					bytes.emplace_back();
					if (firstReadError.isEmpty())
						firstReadError = file.errorString();
				}
			}
		}

		if (job._delivery.isCancelled())
			return;
		cpuThreadPool().enqueue([jobPtr, bytes = std::move(bytes), readError = std::move(firstReadError)]() mutable {
			decodeStage(jobPtr, bytes, readError);
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

	auto job = std::make_shared<LoadJob>(context, std::move(done));
	job->_paths = _paths;
	job->_canvasLogical = box;
	job->_dpr = dpr;
#ifdef PROFILING_ENABLED
	job->_batchToken = std::make_unique<LoadBatchToken>();
#endif
	ImageSource::Request request{ std::shared_ptr<ImageSource::Delivery>{ job, &job->_delivery } };
	// Deliberately untagged: blocking retire() per thumbnail would serialize grid teardown; cancelling makes it safe.
	IoThreadPool::enqueue(_paths.front(), [job = std::move(job)] { readStage(job); });
	return request;
}
