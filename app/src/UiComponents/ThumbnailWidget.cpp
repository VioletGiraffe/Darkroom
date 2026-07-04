#include "UiComponents/ThumbnailWidget.h"
#include "Theme/Theme.h"

#include <QApplication>
#include <QDesktopServices>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QRunnable>
#include <QThreadPool>
#include <QUrl>
#include <QWheelEvent>

#include <mutex>
#include <vector>

// Folder preview: gap between cells in the composite strip
static constexpr int FOLDER_PREVIEW_GAP = 2;
static constexpr int THUMBNAIL_LABEL_HEIGHT = 20;

struct ThumbnailWidget::BaseLoaderTask : public QRunnable {
	BaseLoaderTask() {
		setAutoDelete(false);
	}
	virtual void disarm() = 0;

	std::mutex m_mutex;
};

namespace {
	// Best-fits each frame into its slot within the given canvas (the max bound), then composes them into
	// a canvas sized to the actual content — so the widget's sizeHint() can tighten to the real bounding box.
	// Rendered at device resolution; gaps are left transparent so the styled background shows through.
	struct ImageLoaderTask final : public ThumbnailWidget::BaseLoaderTask {
		ImageLoaderTask(ThumbnailWidget* parent, QStringList paths, QImage* target, QSize canvasLogical, qreal dpr)
			: m_paths{ std::move(paths) }, m_target{ target }, m_parent{ parent }, m_canvasLogical{ canvasLogical }, m_dpr{ dpr }
		{
		}
		void run() override {
			std::lock_guard lock{ m_mutex };
			if (!m_parent || m_paths.isEmpty() || m_canvasLogical.isEmpty() || m_dpr <= 0.0)
				return;

			const int n = static_cast<int>(m_paths.size());
			const int slotWidth = (m_canvasLogical.width() - (n - 1) * FOLDER_PREVIEW_GAP) / n;
			const int slotHeight = m_canvasLogical.height();
			if (slotWidth <= 0 || slotHeight <= 0)
				return;

			// Best-fit each frame into its slot, then resample its pixel data to that exact physical size
			// right here via QImage::scaled() — Qt's dedicated area-correct minification filter. Drawing
			// the full-resolution source straight into the slot via QPainter instead would use the paint
			// engine's transform-time bilinear sampling, which aliases badly at the size reductions involved
			// here (source frames are typically ~1080p, slots a couple hundred px) — that's what made an
			// earlier version of this code look pixelated despite being "the right size". logicalSize still
			// drives all the layout math below; only the pixel data itself is produced at physical resolution.
			struct Fitted { QImage img; QSize logicalSize; };
			std::vector<Fitted> fitted;
			fitted.reserve(n);
			for (const QString& path : std::as_const(m_paths))
			{
				QImage src(path);
				if (!src.isNull())
				{
					const QSize logicalSize = src.size().scaled(QSize(slotWidth, slotHeight), Qt::KeepAspectRatio);
					const QSize physicalSize = (QSizeF(logicalSize) * m_dpr).toSize();
					fitted.push_back({ src.scaled(physicalSize, Qt::KeepAspectRatio, Qt::SmoothTransformation), logicalSize });
				}
			}

			if (fitted.empty())
			{
				// Nothing to show (e.g. a cleared frame). Leave m_image null but still repaint so the
				// previously shown frame is removed rather than left stale on screen.
				QMetaObject::invokeMethod(m_parent, [parent = m_parent] {
					parent->updateGeometry();
					parent->update();
				}, Qt::QueuedConnection);
				return;
			}

			// Tight composite: width = sum of fitted (logical) widths + gaps, height = tallest fitted frame.
			int totalWidth = (static_cast<int>(fitted.size()) - 1) * FOLDER_PREVIEW_GAP;
			int totalHeight = 0;
			for (const Fitted& f : std::as_const(fitted))
			{
				totalWidth += f.logicalSize.width();
				totalHeight = qMax(totalHeight, f.logicalSize.height());
			}

			QImage canvas((QSizeF(totalWidth, totalHeight) * m_dpr).toSize(), QImage::Format_ARGB32_Premultiplied);
			canvas.fill(Qt::transparent);
			canvas.setDevicePixelRatio(m_dpr); // lets us paint below in logical coordinates

			QPainter painter(&canvas);
			painter.setRenderHint(QPainter::SmoothPixmapTransform, true); // defensive only: f.img is already ~exactly logicalSize*dpr, so this draw is normally a 1:1 copy, not a real resample
			int x = 0;
			for (const Fitted& f : std::as_const(fitted))
			{
				painter.drawImage(QRect(QPoint(x, (totalHeight - f.logicalSize.height()) / 2), f.logicalSize), f.img);
				x += f.logicalSize.width() + FOLDER_PREVIEW_GAP;
			}
			painter.end();
			*m_target = std::move(canvas);

			// sizeHint() now reflects the loaded image, so the layout must re-query it, not just repaint.
			QMetaObject::invokeMethod(m_parent, [parent = m_parent] {
				parent->updateGeometry();
				parent->update();
			}, Qt::QueuedConnection);
		}

		void disarm() override {
			m_parent = nullptr;
			m_target = nullptr;
			m_paths.clear();
		}

		QStringList m_paths;
		QImage* m_target = nullptr;
		ThumbnailWidget* m_parent = nullptr;
		QSize m_canvasLogical;
		qreal m_dpr = 1.0;
	};

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

ThumbnailWidget::ThumbnailWidget(const QStringList& compositePaths, const QString& label, QWidget* parent, QSize canvasSize, bool dynamicSizeHint, bool framed)
	: QWidget(parent)
	, m_caption{ label }
	, m_bDynamicSizeHint{ dynamicSizeHint }
	, m_framed{ framed }
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

void ThumbnailWidget::scheduleRender()
{
	disarmAsyncTask();  // no-op if none is scheduled yet (first render); otherwise cancels the in-flight one

	// devicePixelRatioF() is only trustworthy once the widget is realized on its real, final screen. Grid/
	// search cards are constructed parentless and reparented afterward, so this can capture a stale ratio
	// (e.g. 1.0 on a 150%-scaled display) the first time round; paintEvent re-checks and re-renders if so.
	m_renderDpr = devicePixelRatioF();
	m_asyncTask = std::make_unique<ImageLoaderTask>(this, m_sourcePaths, &m_image, m_maxSize, m_renderDpr);
	QThreadPool::globalInstance()->start(m_asyncTask.get());
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

	// Before the first paint the render hasn't been scheduled yet (m_asyncTask is null) and m_image is still
	// null, so the placeholder m_maxSize is the answer; once scheduled, read m_image under the task's lock.
	QSize imageArea = m_maxSize;
	if (m_asyncTask)
	{
		std::lock_guard lock{ m_asyncTask->m_mutex };
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
	// The render is started lazily on the first paint, not at construction: a QListWidget grid builds a card
	// for every item but only paints the visible ones, so off-screen cards never reach here and never decode
	// their (potentially full-resolution) image until scrolled into view. paintEvent is also the one place
	// devicePixelRatioF() is guaranteed accurate - the widget is on its real, shown screen - so this both
	// kicks off the initial render and re-renders if an earlier scheduleRender() captured a stale DPR.
	if (!m_asyncTask || !qFuzzyCompare(m_renderDpr, devicePixelRatioF()))
		scheduleRender();

	QPainter painter(this);

	const QRect content = contentsRect();
	const int labelHeight = m_caption.isEmpty() ? 0 : THUMBNAIL_LABEL_HEIGHT;
	const QRect imageArea(content.left(), content.top(), content.width(), content.height() - labelHeight);

	// Lock the image while drawing to prevent it from being modified by the loader thread
	std::lock_guard lock{ m_asyncTask->m_mutex };

	if (!m_image.isNull())
	{
		const QSize imageLogical = m_image.deviceIndependentSize().toSize();
		if (imageLogical.width() <= imageArea.width() && imageLogical.height() <= imageArea.height())
		{
			// The canvas was rendered for (at most) this area — blit it 1:1, centered. No rescale, so a
			// wrong size shows as clipping or a floating gap rather than being silently hidden.
			QRect r(QPoint(0, 0), imageLogical);
			r.moveCenter(imageArea.center());
			painter.drawImage(r.topLeft(), m_image);
		}
		else
		{
			// Transient: the area shrank (e.g. mid-resize) before the re-render landed. Fast-scale to fit.
			painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
			QRect r(QPoint(0, 0), imageLogical.scaled(imageArea.size(), Qt::KeepAspectRatio));
			r.moveCenter(imageArea.center());
			painter.drawImage(r, m_image);
		}
	}
	else
		painter.drawText(content, Qt::AlignCenter, tr("Loading..."));

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
	if (!m_asyncTask)  // never painted (e.g. an off-screen grid card cleared before it was ever shown): nothing was scheduled
		return;

	{
		std::lock_guard lock{ m_asyncTask->m_mutex };
		(void)QThreadPool::globalInstance()->tryTake(m_asyncTask.get()); // We don't care about the result

		m_asyncTask->disarm();
	} // Remember to unlock m_asyncTask->m_mutex before deleting m_asyncTask

	m_asyncTask.reset();
}