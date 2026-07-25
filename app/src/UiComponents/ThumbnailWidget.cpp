#include "UiComponents/ThumbnailWidget.h"
#include "Core/IoThreadPool.h"
#include "Theme/Theme.h"
#include "assert/advanced_assert.h"

#include <QApplication>
#include <QBuffer>
#include <QByteArray>
#include <QColor>
#include <QDesktopServices>
#include <QFile>
#include <QFileInfo>
#include <QImageIOHandler>
#include <QImageReader>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRunnable>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <QWheelEvent>

#include <memory>
#include <mutex>
#include <vector>

static constexpr int FOLDER_PREVIEW_GAP = 2;
static constexpr int THUMBNAIL_LABEL_HEIGHT = 20;

// Avoid reading cards merely flicked past during scrolling.
static constexpr int LOAD_DWELL_MS = 100;

// Shared lifetime spans I/O and decode. Widget pointers and _disarmed are mutex-protected because the GUI may
// disarm while a stage runs; the remaining fields are immutable after posting.
struct ThumbnailWidget::LoadJob {
	std::mutex _mutex;
	ThumbnailWidget* _parent = nullptr;
	QImage* _target = nullptr;
	QString* _errorMsg = nullptr;
	bool _disarmed = false;

	QStringList _paths;
	QSize _canvasLogical;
	qreal _dpr = 1.0;
};

namespace {
	// Decode and compose outside the lock; lock only the final widget-state install.
	struct DecodeStage final : public QRunnable {
		DecodeStage(std::shared_ptr<ThumbnailWidget::LoadJob> job, std::vector<QByteArray> bytes, QString readError)
			: _job{ std::move(job) }, _bytes{ std::move(bytes) }, _readError{ std::move(readError) }
		{
		}

		void run() override {
			ThumbnailWidget::LoadJob& job = *_job;
			if (_bytes.empty() || job._canvasLogical.isEmpty() || job._dpr <= 0.0)
				return;

			const int n = static_cast<int>(_bytes.size());
			const int slotWidth = (job._canvasLogical.width() - (n - 1) * FOLDER_PREVIEW_GAP) / n;
			const int slotHeight = job._canvasLogical.height();
			if (slotWidth <= 0 || slotHeight <= 0)
				return;

			// Decode near the fitted physical size: cheaper and less aliased than full decode + one large scale.
			// Fit in EXIF-oriented dimensions, but pass transposed stored dimensions to the reader.
			struct Fitted { QImage img; QSize logicalSize; };
			std::vector<Fitted> fitted;
			fitted.reserve(n);
			QString firstError = _readError;
			for (int i = 0; i < n; ++i)
			{
				QByteArray& bytes = _bytes[static_cast<size_t>(i)];
				QBuffer buffer(&bytes);
				buffer.open(QIODevice::ReadOnly);
				QImageReader reader(&buffer);  // A format hint would suppress content detection.
				reader.setAutoTransform(true);

				const QSize rawSize = reader.size();
				if (rawSize.isEmpty())
				{
					if (firstError.isEmpty())
						firstError = reader.errorString();
					continue;
				}

				const bool swapsWH = reader.transformation().testFlag(QImageIOHandler::TransformationRotate90);
				const QSize displayedSize = swapsWH ? rawSize.transposed() : rawSize;
				const QSize logicalSize   = displayedSize.scaled(QSize(slotWidth, slotHeight), Qt::KeepAspectRatio);
				const QSize physicalSize  = (QSizeF(logicalSize) * job._dpr).toSize();
				reader.setScaledSize(swapsWH ? physicalSize.transposed() : physicalSize);

				const QImage src = reader.read();
				if (!src.isNull())
					fitted.push_back({ src, logicalSize });
				else if (firstError.isEmpty())
					firstError = reader.errorString();
			}

			// An empty result still installs below, clearing any previously shown frame.
			QImage canvas;
			if (!fitted.empty())
			{
				int totalWidth = (static_cast<int>(fitted.size()) - 1) * FOLDER_PREVIEW_GAP;
				int totalHeight = 0;
				for (const Fitted& f : std::as_const(fitted))
				{
					totalWidth += f.logicalSize.width();
					totalHeight = qMax(totalHeight, f.logicalSize.height());
				}

				canvas = QImage((QSizeF(totalWidth, totalHeight) * job._dpr).toSize(), QImage::Format_ARGB32_Premultiplied);
				canvas.fill(Qt::transparent);
				canvas.setDevicePixelRatio(job._dpr);

				QPainter painter(&canvas);
				painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
				int x = 0;
				for (const Fitted& f : std::as_const(fitted))
				{
					painter.drawImage(QRect(QPoint(x, (totalHeight - f.logicalSize.height()) / 2), f.logicalSize), f.img);
					x += f.logicalSize.width() + FOLDER_PREVIEW_GAP;
				}
			}

			// disarm() shares this lock, so _parent remains valid through the queued invocation.
			std::lock_guard lock{ job._mutex };
			if (job._disarmed || !job._target)
				return;
			if (!fitted.empty())
				*job._target = std::move(canvas);
			else if (job._errorMsg)
			{
				assert_r(!firstError.isEmpty());
				*job._errorMsg = ThumbnailWidget::tr("Failed to load:") + '\n' + firstError;
			}
			QMetaObject::invokeMethod(job._parent, [parent = job._parent] {
				parent->updateGeometry();
				parent->update();
			}, Qt::QueuedConnection);
		}

		std::shared_ptr<ThumbnailWidget::LoadJob> _job;
		std::vector<QByteArray> _bytes;
		QString _readError;
	};

	// The I/O stage touches only the job, then hands bytes to the CPU pool.
	void readStage(const std::shared_ptr<ThumbnailWidget::LoadJob>& jobPtr)
	{
		ThumbnailWidget::LoadJob& job = *jobPtr;
		{
			std::lock_guard lock{ job._mutex };
			if (job._disarmed)
				return;
		}

		std::vector<QByteArray> bytes;
		QString firstReadError;
		bytes.reserve(job._paths.size());
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

		{
			std::lock_guard lock{ job._mutex };
			if (job._disarmed)
				return;
		}
		QThreadPool::globalInstance()->start(new DecodeStage{ jobPtr, std::move(bytes), std::move(firstReadError) });
	}

	// Transparent composite gaps expose the black film base; sprockets occupy the reserved bands.
	void paintFilmBase(QPainter& painter, const QRect& imageArea)
	{
		painter.save();
		painter.setRenderHint(QPainter::Antialiasing);
		painter.setPen(Qt::NoPen);
		painter.setBrush(QColor(18, 18, 18));
		painter.drawRoundedRect(imageArea, Theme::ThumbnailMatteRadius, Theme::ThumbnailMatteRadius);
		painter.restore();
	}

	void paintSprockets(QPainter& painter, const QRect& imageArea, int bandHeight)
	{
		const int holeHeight = qMax(3, bandHeight * 40 / 100);
		const int holeWidth  = qMax(4, holeHeight * 14 / 10);
		const int pitch      = holeWidth * 14 / 5;
		const int count      = qMax(2, imageArea.width() / pitch);
		const int cell       = imageArea.width() / count;
		const int radius     = qMax(1, holeHeight / 3);
		const int topY       = imageArea.top() + (bandHeight - holeHeight) / 2;
		const int bottomY    = imageArea.bottom() - bandHeight + (bandHeight - holeHeight) / 2;

		painter.save();
		painter.setRenderHint(QPainter::Antialiasing);
		painter.setPen(Qt::NoPen);
		painter.setBrush(QColor(233, 231, 225));   // film base showing through the perforations - warm off-white, fixed in both themes
		for (int i = 0; i < count; ++i)
		{
			const int x = imageArea.left() + i * cell + (cell - holeWidth) / 2;
			painter.drawRoundedRect(QRect(x, topY, holeWidth, holeHeight), radius, radius);
			painter.drawRoundedRect(QRect(x, bottomY, holeWidth, holeHeight), radius, radius);
		}
		painter.restore();
	}

} // anonymous namespace

ThumbnailWidget::ThumbnailWidget(const QString& filePath, const QString& label, int thumbnailSize, QWidget* parent)
	: QWidget(parent), _filePath{ filePath }, _caption{ label }
{
	if (thumbnailSize != 0)
		setFixedSize(thumbnailSize, thumbnailSize);

	setContextMenuPolicy(Qt::CustomContextMenu);
	applyStyleSettings();

	// Layout-sized widgets remain empty until loadFrame supplies their first real dimensions.
	_sourcePaths = { _filePath };
	_maxSize = currentImageArea();
	// Only single-file thumbnails provide the default file URL drag.
	_dragMimeDataFactory = [this] {
		auto* mime = new QMimeData();
		mime->setUrls({ QUrl::fromLocalFile(_filePath) });
		return mime;
	};
}

ThumbnailWidget::ThumbnailWidget(const QStringList& compositePaths, const QString& label, QWidget* parent, QSize canvasSize, bool dynamicSizeHint, bool framed, bool filmStrip)
	: QWidget(parent)
	, _caption{ label }
	, _bDynamicSizeHint{ dynamicSizeHint }
	, _framed{ framed }
	, _filmStrip{ filmStrip }
{
	setFocusPolicy(Qt::NoFocus);
	applyStyleSettings();

	_sourcePaths = compositePaths;
	_maxSize = canvasSize;
}

ThumbnailWidget::~ThumbnailWidget()
{
	disarmAsyncTask();
}

void ThumbnailWidget::loadFrame(const QString& path, const QString& caption)
{
	// Keep the old image during a size-only rerender to avoid flashing a loading placeholder.
	const bool contentChanged = (path != _filePath);
	_filePath = path;
	_caption = caption;
	_sourcePaths = { path };
	// Disarm before clearing state protected by the job lock.
	disarmAsyncTask();
	if (contentChanged)
		_image = QImage{};
	_maxSize = currentImageArea();
	scheduleRender();
}

void ThumbnailWidget::setCaption(const QString& caption)
{
	if (_caption == caption)
		return;

	_caption = caption;
	update();
}

QSize ThumbnailWidget::currentImageArea() const
{
	const QRect c = contentsRect();
	const int labelHeight = _caption.isEmpty() ? 0 : THUMBNAIL_LABEL_HEIGHT;
	return QSize(c.width(), c.height() - labelHeight);
}

int ThumbnailWidget::filmStripBandHeight(int imageAreaHeight)
{
	// Scale with zoom but cap the bands at both extremes.
	return qBound(7, imageAreaHeight / 11, 18);
}

void ThumbnailWidget::scheduleRender()
{
	disarmAsyncTask();

	// Parentless cards may capture a stale DPR before realization; paintEvent rechecks it on the real screen.
	_renderDpr = devicePixelRatioF();

	_errorMessage.clear();

	_job = std::make_shared<LoadJob>();
	_job->_parent = this;
	_job->_target = &_image;
	_job->_errorMsg = &_errorMessage;
	_job->_paths = _sourcePaths;
	// Film frames render between the bands while _maxSize retains the full card footprint.
	_job->_canvasLogical = _filmStrip
		? QSize(_maxSize.width(), qMax(1, _maxSize.height() - 2 * filmStripBandHeight(_maxSize.height())))
		: _maxSize;
	_job->_dpr = _renderDpr;
	// Deliberately untagged: blocking retire() per thumbnail would serialize grid teardown; disarming makes it safe.
	IoThreadPool::enqueue(_sourcePaths.value(0), [job = _job] { readStage(job); });
}

void ThumbnailWidget::setOnMouseWheelCallback(std::function<void(int)> handler)
{
	_onZoomRequested = std::move(handler);
}

QSize ThumbnailWidget::sizeHint() const
{
	if (!_bDynamicSizeHint)
		return _maxSize;

	// Dynamic consumers tighten after load; QListWidget callers must update their cached item hint themselves.
	const QMargins m = contentsMargins();
	const int labelHeight = _caption.isEmpty() ? 0 : THUMBNAIL_LABEL_HEIGHT;

	// Before scheduling, _maxSize is the placeholder; afterward _image shares the job mutex with decoding.
	QSize imageArea = _maxSize;
	if (_job)
	{
		std::lock_guard lock{ _job->_mutex };
		if (!_image.isNull())
			imageArea = _image.deviceIndependentSize().toSize();
	}
	return QSize(imageArea.width() + m.left() + m.right(),
	            imageArea.height() + labelHeight + m.top() + m.bottom());
}

void ThumbnailWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
	if (event->button() == Qt::LeftButton && !_filePath.isEmpty())
	{
		if (openFile())
		{
			// Hide every app window while the external viewer is active.
			for (QWidget* w : QApplication::topLevelWidgets())
				if (w->isVisible() && w->windowType() == Qt::Window)
					w->showMinimized();
		}
	}
	QWidget::mouseDoubleClickEvent(event);
}

void ThumbnailWidget::mousePressEvent(QMouseEvent* event)
{
	_dragHelper.mousePressed(event);
	QWidget::mousePressEvent(event);
}

void ThumbnailWidget::mouseMoveEvent(QMouseEvent* event)
{
	if (_dragMimeDataFactory)
	{
		if (_dragHelper.tryStartDrag(this, event, _dragMimeDataFactory, Qt::CopyAction))
			return;
	}
	QWidget::mouseMoveEvent(event);
}

void ThumbnailWidget::wheelEvent(QWheelEvent* event)
{
	const int dy = event->angleDelta().y();
	if (_onZoomRequested && (event->modifiers() & Qt::ControlModifier) && dy != 0)
	{
		_onZoomRequested(dy > 0 ? 1 : -1);
		event->accept();
		return;
	}
	QWidget::wheelEvent(event);
}

void ThumbnailWidget::paintEvent(QPaintEvent*)
{
	// First paint arms one dwell timer. Only a still-visible card reads disk; empty sources remain placeholders.
	if (!_job && !_sourcePaths.isEmpty())
	{
		if (!_loadArmed)
		{
			_loadArmed = true;
			QTimer::singleShot(LOAD_DWELL_MS, this, [this] {
				_loadArmed = false;
				if (!_job && !visibleRegion().isEmpty())
					scheduleRender();
			});
		}
	}
	// Correct a stale pre-realization DPR immediately; this shown widget now reports the authoritative value.
	else if (_job && !qFuzzyCompare(_renderDpr, devicePixelRatioF()))
		scheduleRender();

	QPainter painter(this);

	const QRect content = contentsRect();
	const int labelHeight = _caption.isEmpty() ? 0 : THUMBNAIL_LABEL_HEIGHT;
	const QRect imageArea(content.left(), content.top(), content.width(), content.height() - labelHeight);

	const int bandHeight = _filmStrip ? filmStripBandHeight(imageArea.height()) : 0;
	if (_filmStrip)
		paintFilmBase(painter, imageArea);

	// Once a job exists, decoding and painting share its image lock.
	std::unique_lock<std::mutex> lock;
	if (_job)
		lock = std::unique_lock{ _job->_mutex };

	if (!_image.isNull())
	{
		// Blit 1:1 so sizing mistakes remain visible; only transient oversize during rerender scales down.
		const QSize imageLogical = _image.deviceIndependentSize().toSize();
		const bool fits = imageLogical.width() <= imageArea.width() && imageLogical.height() <= imageArea.height();
		QRect r(QPoint(0, 0), fits ? imageLogical : imageLogical.scaled(imageArea.size(), Qt::KeepAspectRatio));
		r.moveCenter(imageArea.center());

		const auto blit = [&] {
			if (fits)
				painter.drawImage(r.topLeft(), _image);
			else
			{
				painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
				painter.drawImage(r, _image);
			}
		};

		if (_filmStrip)
			// Reserved bands keep square frame corners inside the rounded film base.
			blit();
		else
		{
			// QSS border-radius does not clip custom painting, so clip the image explicitly to the matte.
			painter.save();
			QPainterPath roundedImage;
			roundedImage.addRoundedRect(QRectF(r), Theme::ThumbnailMatteRadius, Theme::ThumbnailMatteRadius);
			painter.setRenderHint(QPainter::Antialiasing);
			painter.setClipPath(roundedImage);
			blit();
			painter.restore();
		}
	}
	else if (!_errorMessage.isEmpty())
		painter.drawText(content, Qt::AlignCenter | Qt::TextWordWrap, _errorMessage);
	else if (_sourcePaths.isEmpty())
		painter.drawText(content, Qt::AlignCenter, tr("No preview"));
	else
		painter.drawText(content, Qt::AlignCenter, tr("Loading..."));

	if (_filmStrip)
		paintSprockets(painter, imageArea, bandHeight);

	if (!_caption.isEmpty())
	{
		const QRect labelRect(content.left() + 2, content.bottom() - THUMBNAIL_LABEL_HEIGHT + 1, content.width() - 4, THUMBNAIL_LABEL_HEIGHT);
		painter.drawText(labelRect, Qt::AlignVCenter | Qt::AlignHCenter, painter.fontMetrics().elidedText(_caption, Qt::ElideMiddle, labelRect.width()));
	}
}

void ThumbnailWidget::applyStyleSettings()
{
	// QWidget does not derive contentsMargins from QSS border/padding; mirror the central sheet manually.
	static constexpr int THUMBNAIL_BORDER_WIDTH = 1;
	static constexpr int THUMBNAIL_PADDING = 2;

	setAttribute(Qt::WA_StyledBackground);

	// Object-name rules in the central sheet avoid per-instance polish across hundreds of thumbnails.
	if (!_framed)
	{
		setObjectName("cardThumbnailWell");
		setContentsMargins(0, 0, 0, 0);
		return;
	}

	setObjectName("framedThumbnail");
	setAttribute(Qt::WA_Hover);
	setContentsMargins(
		THUMBNAIL_BORDER_WIDTH + THUMBNAIL_PADDING,
		THUMBNAIL_BORDER_WIDTH + THUMBNAIL_PADDING,
		THUMBNAIL_BORDER_WIDTH + THUMBNAIL_PADDING,
		THUMBNAIL_BORDER_WIDTH + THUMBNAIL_PADDING);
}

bool ThumbnailWidget::openFile()
{
	if (QDesktopServices::openUrl(QUrl::fromLocalFile(_filePath)))
		return true;

	QMessageBox::warning(this, tr("Error"), tr("Failed to open file:\n%1").arg(_filePath));
	return false;
}

void ThumbnailWidget::disarmAsyncTask()
{
	if (!_job)
		return;

	{
		// After this lock, running stages cannot write widget state and queued stages skip their work.
		std::lock_guard lock{ _job->_mutex };
		_job->_disarmed = true;
		_job->_parent = nullptr;
		_job->_target = nullptr;
		_job->_errorMsg = nullptr;
	}
	_job.reset();
}
