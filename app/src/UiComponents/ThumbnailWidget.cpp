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

// Folder preview: gap between cells in the composite strip
static constexpr int FOLDER_PREVIEW_GAP = 2;
static constexpr int THUMBNAIL_LABEL_HEIGHT = 20;

// Grace period after a card first becomes visible before its image load starts, so flicking past a card doesn't
// trigger a decode (and disk read) for something the user isn't looking at. See paintEvent.
static constexpr int LOAD_DWELL_MS = 100;

// Shared control block for one thumbnail load, split across two stages (read on the single I/O thread, decode on
// the CPU pool). It's held by a shared_ptr so it outlives the widget when a stage is still running: the widget's
// destructor disarms it (below) rather than deleting it, and each stage re-checks m_disarmed before touching the
// widget. m_parent/m_target/m_disarmed are guarded by m_mutex (disarm() flips them from the GUI thread while a
// stage may run); m_paths/m_canvasLogical/m_dpr are set once before posting and only read after, so lock-free.
struct ThumbnailWidget::LoadJob {
	std::mutex m_mutex;
	ThumbnailWidget* m_parent = nullptr;
	QImage* m_target = nullptr;         // = &widget->m_image
	QString* m_errorMsg = nullptr;      // = &widget->m_errorMessage
	bool m_disarmed = false;

	QStringList m_paths;                // source frame path(s), read by the read stage
	QSize m_canvasLogical;
	qreal m_dpr = 1.0;
};

namespace {
	// Stage 2 (CPU pool, multi-threaded): decode the bytes the read stage produced, best-fit each frame into its
	// slot and compose a canvas sized to the actual content (so sizeHint() can tighten to the real bounding box),
	// then install it into the widget's m_image and repaint. The heavy decode/compose runs outside the lock; the
	// lock is taken only for the brief install, so paintEvent (which reads m_image under the same lock) barely waits.
	struct DecodeStage final : public QRunnable {
		DecodeStage(std::shared_ptr<ThumbnailWidget::LoadJob> job, std::vector<QByteArray> bytes, QString readError)
			: m_job{ std::move(job) }, m_bytes{ std::move(bytes) }, m_readError{ std::move(readError) }
		{
		}

		void run() override {
			ThumbnailWidget::LoadJob& job = *m_job;
			if (m_bytes.empty() || job.m_canvasLogical.isEmpty() || job.m_dpr <= 0.0)
				return;

			const int n = static_cast<int>(m_bytes.size());
			const int slotWidth = (job.m_canvasLogical.width() - (n - 1) * FOLDER_PREVIEW_GAP) / n;
			const int slotHeight = job.m_canvasLogical.height();
			if (slotWidth <= 0 || slotHeight <= 0)
				return;

			// Decode each frame straight to its fitted physical size via QImageReader::setScaledSize rather than
			// full-decode-then-QImage::scaled: for JPEG this is a reduced-scale libjpeg decode (the bulk of the
			// reduction done by a good filter), which is far cheaper and also sidesteps the minification aliasing a
			// single large downscale used to cause. setAutoTransform applies EXIF orientation. Both size() and
			// setScaledSize are in the file's *stored* orientation, and the transform is applied to the result, so
			// for a 90/270 rotation the displayed image is transposed: fit against the displayed size, but hand the
			// reader the stored-orientation target. logicalSize drives the layout math below; the pixel data is
			// produced at physical resolution.
			struct Fitted { QImage img; QSize logicalSize; };
			std::vector<Fitted> fitted;
			fitted.reserve(n);
			QString firstError = m_readError;
			for (int i = 0; i < n; ++i)
			{
				QByteArray& bytes = m_bytes[static_cast<size_t>(i)];   // non-const: QBuffer wraps a QByteArray*, so it can't be a const ref
				QBuffer buffer(&bytes);
				buffer.open(QIODevice::ReadOnly);
				// The suffix is only a hint (that plugin is tried first); content detection remains the fallback
				QImageReader reader(&buffer, QFileInfo(job.m_paths[i]).suffix().toLower().toUtf8());
				reader.setAutoTransform(true);

				const QSize rawSize = reader.size();   // header only, no full decode
				if (rawSize.isEmpty())
				{
					if (firstError.isEmpty())
						firstError = reader.errorString();
					continue;
				}

				const bool swapsWH = reader.transformation().testFlag(QImageIOHandler::TransformationRotate90);
				const QSize displayedSize = swapsWH ? rawSize.transposed() : rawSize;
				const QSize logicalSize   = displayedSize.scaled(QSize(slotWidth, slotHeight), Qt::KeepAspectRatio);
				const QSize physicalSize  = (QSizeF(logicalSize) * job.m_dpr).toSize();
				reader.setScaledSize(swapsWH ? physicalSize.transposed() : physicalSize);

				const QImage src = reader.read();   // reduced-scale decode + EXIF transform -> physicalSize, displayed orientation
				if (!src.isNull())
					fitted.push_back({ src, logicalSize });
				else if (firstError.isEmpty())
					firstError = reader.errorString();
			}

			// Nothing decoded (e.g. a cleared frame): leave m_image as-is but still repaint below so the previously
			// shown frame is removed rather than left stale. Otherwise compose the tight canvas.
			QImage canvas;
			if (!fitted.empty())
			{
				// Tight composite: width = sum of fitted (logical) widths + gaps, height = tallest fitted frame.
				int totalWidth = (static_cast<int>(fitted.size()) - 1) * FOLDER_PREVIEW_GAP;
				int totalHeight = 0;
				for (const Fitted& f : std::as_const(fitted))
				{
					totalWidth += f.logicalSize.width();
					totalHeight = qMax(totalHeight, f.logicalSize.height());
				}

				canvas = QImage((QSizeF(totalWidth, totalHeight) * job.m_dpr).toSize(), QImage::Format_ARGB32_Premultiplied);
				canvas.fill(Qt::transparent);
				canvas.setDevicePixelRatio(job.m_dpr); // lets us paint below in logical coordinates

				QPainter painter(&canvas);
				painter.setRenderHint(QPainter::SmoothPixmapTransform, true); // defensive only: f.img is already ~exactly logicalSize*dpr, so this draw is normally a 1:1 copy, not a real resample
				int x = 0;
				for (const Fitted& f : std::as_const(fitted))
				{
					painter.drawImage(QRect(QPoint(x, (totalHeight - f.logicalSize.height()) / 2), f.logicalSize), f.img);
					x += f.logicalSize.width() + FOLDER_PREVIEW_GAP;
				}
			}

			// Install + repaint under the lock. disarm() takes this same mutex, so within it m_parent is guaranteed
			// alive for the invokeMethod; a disarmed job (widget torn down, or superseded by a newer render) drops
			// the result. Heavy work is already done above, so this critical section is just a move + an event post.
			std::lock_guard lock{ job.m_mutex };
			if (job.m_disarmed || !job.m_target)
				return;
			if (!fitted.empty())
				*job.m_target = std::move(canvas);
			else if (job.m_errorMsg)
			{
				assert_r(!firstError.isEmpty());
				*job.m_errorMsg = ThumbnailWidget::tr("Failed to load:") + '\n' + firstError;
			}
			// The image (and thus sizeHint()) changed, so the layout must re-query it, not just repaint.
			QMetaObject::invokeMethod(job.m_parent, [parent = job.m_parent] {
				parent->updateGeometry();
				parent->update();
			}, Qt::QueuedConnection);
		}

		std::shared_ptr<ThumbnailWidget::LoadJob> m_job;
		std::vector<QByteArray> m_bytes;
		QString m_readError;
	};

	// Stage 1 (I/O pool): read each source file into memory, then hand the bytes to the decode pool. It touches
	// only the job (never the widget), so it is safe whatever the widget's lifetime. On seek-penalty media the
	// pool serializes the reads onto one thread (no seek-thrashing, OS read-ahead works one file at a time).
	void readStage(const std::shared_ptr<ThumbnailWidget::LoadJob>& jobPtr)
	{
		ThumbnailWidget::LoadJob& job = *jobPtr;
		{
			std::lock_guard lock{ job.m_mutex };
			if (job.m_disarmed)   // superseded, or the widget went away, before the I/O thread reached us: skip the read entirely
				return;
		}

		std::vector<QByteArray> bytes;
		QString firstReadError;
		bytes.reserve(job.m_paths.size());
		for (const QString& path : std::as_const(job.m_paths))
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
			std::lock_guard lock{ job.m_mutex };
			if (job.m_disarmed)   // disarmed while we were reading: don't bother decoding
				return;
		}
		QThreadPool::globalInstance()->start(new DecodeStage{ jobPtr, std::move(bytes), std::move(firstReadError) });  // decode on the shared CPU pool
	}

	// Film-strip chrome, painted by paintEvent over a video card. paintFilmBase lays the black film base across the
	// whole image area (the frame composite's transparent gaps then read as black separators); paintSprockets punches
	// evenly-spaced light perforations into the reserved top and bottom bands.
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
		const int pitch      = holeWidth * 14 / 5;   // hole plus a wide gap (~2.6x the hole width) -> sparse, well-separated perforations
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
	: QWidget(parent), m_filePath{ filePath }, m_caption{ label }
{
	if (thumbnailSize != 0)
		setFixedSize(thumbnailSize, thumbnailSize);

	setContextMenuPolicy(Qt::CustomContextMenu);
	applyStyleSettings();

	// With a fixed size set, contentsRect() is already valid, so the image area is known. Size-0 widgets
	// (e.g. CompareWindow's, sized by their layout) stay empty until the first loadFrame().
	m_sourcePaths = { m_filePath };
	m_maxSize = currentImageArea();
	// The render is deferred to the first paintEvent, not started here - see paintEvent (lazy thumbnail load).

	// Default drag payload: a file:// URL to the displayed file. Composite/folder-preview widgets have no
	// filePath() of their own and aren't drag sources at all (m_dragMimeDataFactory stays unset for them).
	m_dragMimeDataFactory = [this] {
		auto* mime = new QMimeData();
		mime->setUrls({ QUrl::fromLocalFile(m_filePath) });
		return mime;
	};
}

ThumbnailWidget::ThumbnailWidget(const QStringList& compositePaths, const QString& label, QWidget* parent, QSize canvasSize, bool dynamicSizeHint, bool framed, bool filmStrip)
	: QWidget(parent)
	, m_caption{ label }
	, m_bDynamicSizeHint{ dynamicSizeHint }
	, m_framed{ framed }
	, m_filmStrip{ filmStrip }
{
	setFocusPolicy(Qt::NoFocus);
	applyStyleSettings();

	m_sourcePaths = compositePaths;
	m_maxSize = compositePaths.isEmpty() ? QSize{} : canvasSize;
	// Deferred to the first paintEvent, not started here - see paintEvent (lazy thumbnail load).
}

ThumbnailWidget::~ThumbnailWidget()
{
	disarmAsyncTask();
}

void ThumbnailWidget::loadFrame(const QString& path, const QString& caption)
{
	// Only drop the current image when the frame actually changes; on a pure re-render at a new size we
	// keep it so paintEvent can fast-scale it during the transient instead of flashing "Loading...".
	const bool contentChanged = (path != m_filePath);
	m_filePath = path;
	m_caption = caption;
	m_sourcePaths = { path };
	// Disarm any in-flight load before clearing m_image: the decode stage writes m_image under the job lock, so a
	// bare assignment here would race a running decode (scheduleRender() below disarms again - a no-op by then).
	disarmAsyncTask();
	if (contentChanged)
		m_image = QImage{};
	m_maxSize = currentImageArea();
	scheduleRender();
}

void ThumbnailWidget::setCaption(const QString& caption)
{
	if (m_caption == caption)
		return;

	// The caption strip height is unchanged (it stays non-empty), so the rendered image area and
	// sizeHint() are unaffected - a repaint suffices, no re-render or relayout.
	m_caption = caption;
	update();
}

QSize ThumbnailWidget::currentImageArea() const
{
	const QRect c = contentsRect();
	const int labelHeight = m_caption.isEmpty() ? 0 : THUMBNAIL_LABEL_HEIGHT;
	return QSize(c.width(), c.height() - labelHeight);
}

int ThumbnailWidget::filmStripBandHeight(int imageAreaHeight)
{
	// Scaled to the card so the bands stay proportional across zoom, bounded so they never dominate a small
	// card or read as a thick letterbox on a large one.
	return qBound(7, imageAreaHeight / 11, 18);
}

void ThumbnailWidget::scheduleRender()
{
	disarmAsyncTask();  // no-op if none is scheduled yet (first render); otherwise disarms the in-flight one

	// devicePixelRatioF() is only trustworthy once the widget is realized on its real, final screen. Grid/
	// search cards are constructed parentless and reparented afterward, so this can capture a stale ratio
	// (e.g. 1.0 on a 150%-scaled display) the first time round; paintEvent re-checks and re-renders if so.
	m_renderDpr = devicePixelRatioF();

	m_errorMessage.clear();

	m_job = std::make_shared<LoadJob>();
	m_job->m_parent = this;
	m_job->m_target = &m_image;
	m_job->m_errorMsg = &m_errorMessage;
	m_job->m_paths = m_sourcePaths;
	// Film-strip cards reserve top and bottom bands, so frames are composited into a shorter canvas; the full
	// m_maxSize still drives the widget size / sizeHint, and paintEvent centers the shorter strip between the bands.
	m_job->m_canvasLogical = m_filmStrip
		? QSize(m_maxSize.width(), qMax(1, m_maxSize.height() - 2 * filmStripBandHeight(m_maxSize.height())))
		: m_maxSize;
	m_job->m_dpr = m_renderDpr;
	IoThreadPool::enqueue(m_sourcePaths.value(0), [job = m_job] { readStage(job); });   // stage 1 (read) on the I/O pool; it posts stage 2 (decode) to the CPU pool
}

void ThumbnailWidget::setOnMouseWheelCallback(std::function<void(int)> handler)
{
	m_onZoomRequested = std::move(handler);
}

QSize ThumbnailWidget::sizeHint() const
{
	if (!m_bDynamicSizeHint)
		return m_maxSize;

	// m_maxSize (the canvas / max bound) is the placeholder until the image loads; afterwards it's the
	// image's true bounding box, so layout-driven consumers tighten to the actual content. The QListWidget
	// grid caches item sizes and won't re-query — it keeps the placeholder unless a caller updates the
	// item's sizeHint on load (deferred work).
	const QMargins m = contentsMargins();
	const int labelHeight = m_caption.isEmpty() ? 0 : THUMBNAIL_LABEL_HEIGHT;

	// Before the first paint the render hasn't been scheduled yet (m_job is null) and m_image is still null, so
	// the placeholder m_maxSize is the answer; once scheduled, read m_image under the job's lock (the decode
	// stage writes it under the same mutex).
	QSize imageArea = m_maxSize;
	if (m_job)
	{
		std::lock_guard lock{ m_job->m_mutex };
		if (!m_image.isNull())
			imageArea = m_image.deviceIndependentSize().toSize();
	}
	return QSize(imageArea.width() + m.left() + m.right(),
	            imageArea.height() + labelHeight + m.top() + m.bottom());
}

void ThumbnailWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
	if (event->button() == Qt::LeftButton && !m_filePath.isEmpty())
	{
		if (openFile())
		{
			// Handing off to the external viewer: get the whole app out of its way - every window (main,
			// frame viewer, compare, players), not just the one hosting this thumbnail.
			for (QWidget* w : QApplication::topLevelWidgets())
				if (w->isVisible() && w->windowType() == Qt::Window)
					w->showMinimized();
		}
	}
	QWidget::mouseDoubleClickEvent(event);
}

void ThumbnailWidget::mousePressEvent(QMouseEvent* event)
{
	m_dragHelper.mousePressed(event);
	QWidget::mousePressEvent(event);
}

void ThumbnailWidget::mouseMoveEvent(QMouseEvent* event)
{
	if (m_dragMimeDataFactory)
	{
		// Only the single-frame constructor ever sets m_dragMimeDataFactory now (composite/folder-preview
		// thumbnails have no drag payload at all), and that default is always a plain file:// URL copy.
		if (m_dragHelper.tryStartDrag(this, event, m_dragMimeDataFactory, Qt::CopyAction))
			return;
	}
	QWidget::mouseMoveEvent(event);
}

void ThumbnailWidget::wheelEvent(QWheelEvent* event)
{
	const int dy = event->angleDelta().y();
	if (m_onZoomRequested && (event->modifiers() & Qt::ControlModifier) && dy != 0)
	{
		m_onZoomRequested(dy > 0 ? 1 : -1);
		event->accept();
		return;
	}
	QWidget::wheelEvent(event); // plain wheel: leave it to propagate so the surrounding view scrolls
}

void ThumbnailWidget::paintEvent(QPaintEvent*)
{
	// The initial render is deferred to the first paint (a grid builds a card per item but only paints the
	// visible ones) and then gated behind a short dwell: a fast flick paints each card once and scrolls it away
	// before the timer fires, so cards the user didn't stop on never touch the disk. The render starts only if
	// the card is still visible when the timer fires; m_loadArmed stops repaints from stacking timers.
	if (!m_job)
	{
		if (!m_loadArmed)
		{
			m_loadArmed = true;
			QTimer::singleShot(LOAD_DWELL_MS, this, [this] {
				m_loadArmed = false;
				if (!m_job && !visibleRegion().isEmpty())  // still not loaded, and still on-screen after the dwell
					scheduleRender();
			});
		}
	}
	// An already-rendered card that captured a stale DPR earlier (see scheduleRender) re-renders immediately -
	// no dwell: it's correcting something already on-screen, not a fresh decode. paintEvent is the one place
	// devicePixelRatioF() is guaranteed accurate - the widget is on its real, shown screen.
	else if (!qFuzzyCompare(m_renderDpr, devicePixelRatioF()))
		scheduleRender();

	QPainter painter(this);

	const QRect content = contentsRect();
	const int labelHeight = m_caption.isEmpty() ? 0 : THUMBNAIL_LABEL_HEIGHT;
	const QRect imageArea(content.left(), content.top(), content.width(), content.height() - labelHeight);

	// Film-strip video card: lay the black film base first, so the frame composite's transparent inter-frame gaps
	// (and the reserved top/bottom bands) read as black. The frames are rendered shorter (see scheduleRender) and
	// centered below, landing between the bands; the sprockets are punched into the bands last.
	const int bandHeight = m_filmStrip ? filmStripBandHeight(imageArea.height()) : 0;
	if (m_filmStrip)
		paintFilmBase(painter, imageArea);

	// Lock the image while drawing so the decode stage can't swap it mid-paint. During the dwell before the first
	// render there is no job yet (and m_image is null, touched only by this thread), so there is nothing to lock
	// against - fall through unlocked and draw the "Loading..." placeholder.
	std::unique_lock<std::mutex> lock;
	if (m_job)
		lock = std::unique_lock{ m_job->m_mutex };

	if (!m_image.isNull())
	{
		// The canvas was rendered for (at most) this area — blit it 1:1, centered. No rescale, so a wrong
		// size shows as clipping or a floating gap rather than being silently hidden. A too-large canvas is
		// transient (the area shrank, e.g. mid-resize, before the re-render landed): fast-scale it to fit.
		const QSize imageLogical = m_image.deviceIndependentSize().toSize();
		const bool fits = imageLogical.width() <= imageArea.width() && imageLogical.height() <= imageArea.height();
		QRect r(QPoint(0, 0), fits ? imageLogical : imageLogical.scaled(imageArea.size(), Qt::KeepAspectRatio));
		r.moveCenter(imageArea.center());

		const auto blit = [&] {
			if (fits)
				painter.drawImage(r.topLeft(), m_image);
			else
			{
				painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
				painter.drawImage(r, m_image);
			}
		};

		if (m_filmStrip)
			// The frames are windows in the already-rounded black base, inset from its corners by the bands, so
			// their square corners never reach the rounding - no per-frame clip needed, and the gaps stay black.
			blit();
		else
		{
			// Clip the image to softly rounded corners: the well/frame around it is rounded, but QSS
			// border-radius can't clip child painting, so a flush image's square corners would overpaint that
			// rounding. ThumbnailMatteRadius is the borderless well's own radius (the flush case); the framed
			// variant's image sits inset from its frame anyway, where the same small rounding reads consistent.
			painter.save();
			QPainterPath roundedImage;
			roundedImage.addRoundedRect(QRectF(r), Theme::ThumbnailMatteRadius, Theme::ThumbnailMatteRadius);
			painter.setRenderHint(QPainter::Antialiasing);   // antialiases the clip edge, so the corners stay smooth
			painter.setClipPath(roundedImage);
			blit();
			painter.restore();   // the caption below must not inherit the clip
		}
	}
	else if (!m_errorMessage.isEmpty())
		painter.drawText(content, Qt::AlignCenter | Qt::TextWordWrap, m_errorMessage);
	else
		painter.drawText(content, Qt::AlignCenter, tr("Loading..."));

	if (m_filmStrip)
		paintSprockets(painter, imageArea, bandHeight);

	if (!m_caption.isEmpty())
	{
		const QRect labelRect(content.left() + 2, content.bottom() - THUMBNAIL_LABEL_HEIGHT + 1, content.width() - 4, THUMBNAIL_LABEL_HEIGHT);
		painter.drawText(labelRect, Qt::AlignVCenter | Qt::AlignHCenter, painter.fontMetrics().elidedText(m_caption, Qt::ElideMiddle, labelRect.width()));
	}
}

void ThumbnailWidget::applyStyleSettings()
{
	// Plain QWidget has no QStyle sub-element for its box model, so the stylesheet engine never syncs contentsMargins() from border/padding the way it does for QPushButton etc.
	// So we must manually apply the content margins that match those in the central stylesheet.
	static constexpr int THUMBNAIL_BORDER_WIDTH = 1;
	static constexpr int THUMBNAIL_PADDING = 2;

	setAttribute(Qt::WA_StyledBackground);

	// The visual style lives in the central sheet (Style.cpp), keyed by these object names, rather than a
	// per-instance setStyleSheet: the latter polishes slowly, and both the card grid and the frame viewer create
	// these in bulk (hundreds at a time).
	if (!m_framed)
	{
		// Borderless: just the recessed matte well. The surrounding MediaItemWidget owns the card's border,
		// background and hover, so the thumbnail carries no frame, no hover and no inset of its own.
		setObjectName("cardThumbnailWell");
		setContentsMargins(0, 0, 0, 0);
		return;
	}

	setObjectName("framedThumbnail");
	setAttribute(Qt::WA_Hover);   // enable :hover tracking so the central #framedThumbnail:hover rule fires
	setContentsMargins(
		THUMBNAIL_BORDER_WIDTH + THUMBNAIL_PADDING,
		THUMBNAIL_BORDER_WIDTH + THUMBNAIL_PADDING,
		THUMBNAIL_BORDER_WIDTH + THUMBNAIL_PADDING,
		THUMBNAIL_BORDER_WIDTH + THUMBNAIL_PADDING);
}

bool ThumbnailWidget::openFile()
{
	if (QDesktopServices::openUrl(QUrl::fromLocalFile(m_filePath)))
		return true;

	QMessageBox::warning(this, tr("Error"), tr("Failed to open file:\n%1").arg(m_filePath));
	return false;
}

void ThumbnailWidget::disarmAsyncTask()
{
	if (!m_job)  // nothing scheduled yet (e.g. an off-screen grid card cleared before it was ever painted)
		return;

	{
		// Flip the flag under the lock: a stage running right now finishes its critical section first (so it
		// can't write m_image after we return), then sees m_disarmed and drops its result; a not-yet-run stage
		// skips the disk read / decode. Nulling the pointers makes any late stage a no-op.
		std::lock_guard lock{ m_job->m_mutex };
		m_job->m_disarmed = true;
		m_job->m_parent = nullptr;
		m_job->m_target = nullptr;
		m_job->m_errorMsg = nullptr;
	}
	// An in-flight stage keeps the job alive through its own shared_ptr; it's freed when that stage ends.
	m_job.reset();
}