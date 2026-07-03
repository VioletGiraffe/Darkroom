#include "Windows/PhotoCompareWindow.h"
#include "Theme/Theme.h"
#include "Utils.h"

#include <QDebug>
#include <QFileInfo>
#include <QGridLayout>
#include <QImageReader>
#include <QKeyEvent>
#include <QLabel>
#include <QLineF>
#include <QMouseEvent>
#include <QPainter>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

// One grid cell: a viewport onto the shared view. All state (images, alignment, the view itself) lives in
// the owning PhotoCompareWindow; the pane renders and translates mouse input into owner calls.
class PhotoComparePane final : public QWidget
{
public:
	PhotoComparePane(PhotoCompareWindow& owner, int index) : QWidget(&owner), m_owner(owner), m_index(index)
	{
		setMinimumSize(50, 50);
		setCursor(m_owner.idleCursor());
	}

protected:
	void paintEvent(QPaintEvent*) override;
	void wheelEvent(QWheelEvent* event) override;
	void mousePressEvent(QMouseEvent* event) override;
	void mouseMoveEvent(QMouseEvent* event) override;
	void mouseReleaseEvent(QMouseEvent* event) override;
	void mouseDoubleClickEvent(QMouseEvent* event) override;
	void resizeEvent(QResizeEvent* event) override { QWidget::resizeEvent(event); m_owner.onPaneResized(); }

private:
	PhotoCompareWindow& m_owner;
	const int m_index;

	// Click-vs-drag: a press starts drag tracking; a release that never crossed the threshold is a click
	// (which is how calibration points are placed - so panning stays available while calibrating).
	QPointF m_pressPos;
	QPointF m_lastDragPos;
	bool m_leftButtonDown = false;
	bool m_dragConfirmed = false;
	bool m_ctrlDrag = false;
};

void PhotoComparePane::paintEvent(QPaintEvent*)
{
	QPainter painter(this);
	painter.fillRect(rect(), QColor(Theme::current().ThumbnailMatte));

	const int renderIndex = m_owner.m_flickerIndex >= 0 ? m_owner.m_flickerIndex : m_index;
	PhotoCompareWindow::Pane& pane = m_owner.m_panes[renderIndex];

	const double effectiveScale = m_owner.m_viewZoom * pane.alignScale;
	double residualScale = 1.0;
	const QImage& source = m_owner.imageForScale(pane, effectiveScale, residualScale);

	painter.save();
	painter.setRenderHint(QPainter::SmoothPixmapTransform);
	painter.translate(m_owner.m_viewZoom * pane.alignOffset + m_owner.m_viewPan);
	painter.scale(residualScale, residualScale);
	painter.drawImage(0, 0, source);
	painter.restore();

	painter.setRenderHint(QPainter::Antialiasing);

	// Calibration crosshairs - always the pane's OWN points (flicker is inert while calibrating).
	if (m_owner.m_calibrating)
	{
		painter.setPen(QPen(QColor(Theme::current().AccentBorder), 2));
		for (const QPointF& imagePos : m_owner.m_panes[m_index].calibPoints)
		{
			const QPointF c = m_owner.widgetFromImage(m_owner.m_panes[m_index], imagePos);
			painter.drawLine(c - QPointF(8, 0), c + QPointF(8, 0));
			painter.drawLine(c - QPointF(0, 8), c + QPointF(0, 8));
		}
	}

	// Corner caption: "2 · name.jpg · 63%" - the leading digit doubles as the pane's flicker key, the
	// percentage is this photo's on-screen scale (100% = 1 image px per widget px), making any compensation
	// difference between panes visible at a glance.
	const QString caption = QString("%1 · %2 · %3%")
		.arg(renderIndex + 1).arg(pane.caption).arg(qRound(effectiveScale * 100.0));
	const QFontMetrics fm = painter.fontMetrics();
	const QRectF textRect(8, 8, fm.horizontalAdvance(caption) + 12, fm.height() + 6);
	painter.setPen(Qt::NoPen);
	painter.setBrush(QColor(0, 0, 0, 150));
	painter.drawRoundedRect(textRect, 4, 4);
	painter.setPen(QColor(0xe8, 0xe8, 0xe8));
	painter.drawText(textRect, Qt::AlignCenter, caption);

	// A flickered pane shows a photo other than its own - flag it with an accent frame.
	if (renderIndex != m_index)
	{
		painter.setPen(QPen(QColor(Theme::current().AccentBorder), 3));
		painter.setBrush(Qt::NoBrush);
		painter.drawRect(rect().adjusted(1, 1, -2, -2));
	}
}

void PhotoComparePane::wheelEvent(QWheelEvent* event)
{
	const double steps = event->angleDelta().y() / 120.0;
	if (steps == 0.0)
		return;
	const double factor = std::pow(1.25, steps);
	if (event->modifiers().testFlag(Qt::ControlModifier))
		m_owner.adjustPaneScale(m_index, factor, event->position());
	else
		m_owner.zoomView(factor, event->position());
	event->accept();
}

void PhotoComparePane::mousePressEvent(QMouseEvent* event)
{
	if (event->button() == Qt::LeftButton)
	{
		m_leftButtonDown = true;
		m_dragConfirmed = false;
		m_ctrlDrag = event->modifiers().testFlag(Qt::ControlModifier);
		m_pressPos = m_lastDragPos = event->position();
	}
	else if (event->button() == Qt::RightButton && m_owner.m_calibrating)
		m_owner.undoCalibrationPoint(m_index);
}

void PhotoComparePane::mouseMoveEvent(QMouseEvent* event)
{
	if (!m_leftButtonDown)
		return;
	const QPointF pos = event->position();
	if (!m_dragConfirmed && (pos - m_pressPos).manhattanLength() > 4.0)
	{
		m_dragConfirmed = true;
		setCursor(Qt::ClosedHandCursor);
	}
	if (m_dragConfirmed)
	{
		if (m_ctrlDrag)
			m_owner.movePaneOffset(m_index, pos - m_lastDragPos);
		else
			m_owner.panView(pos - m_lastDragPos);
	}
	m_lastDragPos = pos;
}

void PhotoComparePane::mouseReleaseEvent(QMouseEvent* event)
{
	if (event->button() != Qt::LeftButton || !m_leftButtonDown)
		return;
	m_leftButtonDown = false;
	setCursor(m_owner.idleCursor());
	if (!m_dragConfirmed && m_owner.m_calibrating)
		m_owner.addCalibrationPoint(m_index, m_owner.imageFromWidget(m_owner.m_panes[m_index], m_pressPos));
}

void PhotoComparePane::mouseDoubleClickEvent(QMouseEvent* event)
{
	// While calibrating, a double-click is just a second press - route it as one so its release goes through
	// the normal click path (addCalibrationPoint's proximity guard swallows the would-be duplicate point).
	if (m_owner.m_calibrating)
		mousePressEvent(event);
	else if (event->button() == Qt::LeftButton)
		m_owner.fitView();
}

// ---------------------------------------------------------------------------------------------------------

PhotoCompareWindow::PhotoCompareWindow(const QStringList& photoPaths, QWidget* parent) : QWidget(parent, Qt::Window)
{
	setWindowTitle(tr("Compare Photos"));
	setFocusPolicy(Qt::StrongFocus);  // the panes never take focus, so key events land here

	for (const QString& path : photoPaths)
	{
		QImageReader reader(path);
		reader.setAutoTransform(true);  // apply the EXIF orientation
		QImage image = reader.read();
		if (image.isNull())
		{
			qWarning() << "PhotoCompareWindow: failed to load" << path << "-" << reader.errorString();
			continue;
		}
		Pane pane;
		pane.image = std::move(image);
		pane.caption = QFileInfo(path).fileName();
		m_panes.push_back(std::move(pane));
	}

	QVBoxLayout* mainLayout = new QVBoxLayout(this);
	mainLayout->setContentsMargins(4, 4, 4, 4);

	if (m_panes.empty())
	{
		QLabel* errorLabel = new QLabel(tr("None of the selected photos could be loaded."), this);
		errorLabel->setAlignment(Qt::AlignCenter);
		mainLayout->addWidget(errorLabel, 1);
	}
	else
	{
		// Default alignment: normalize each photo's height to photo 0's and center them on each other - so
		// identical shots that only differ in export resolution line up with no user action at all. Photo 0
		// itself gets the identity transform (the formula yields exactly that).
		const QSizeF refSize = m_panes[0].image.size();
		for (Pane& pane : m_panes)
		{
			pane.alignScale = refSize.height() / pane.image.height();
			pane.alignOffset = QPointF(refSize.width() - pane.alignScale * pane.image.width(),
			                           refSize.height() - pane.alignScale * pane.image.height()) / 2.0;
		}

		const int paneCount = static_cast<int>(m_panes.size());
		const int columns = static_cast<int>(std::ceil(std::sqrt(paneCount)));
		const int rows = (paneCount + columns - 1) / columns;
		QGridLayout* grid = new QGridLayout();
		grid->setSpacing(4);
		for (int i = 0; i < paneCount; ++i)
		{
			auto* paneWidget = new PhotoComparePane(*this, i);
			m_paneWidgets.push_back(paneWidget);
			grid->addWidget(paneWidget, i / columns, i % columns);
		}
		// Equal stretch keeps every cell the same size (including a 3-way compare's empty 4th cell): the
		// shared pan is in widget coordinates, so equally sized panes is what makes the same widget position
		// show the same subject point in each.
		for (int c = 0; c < columns; ++c)
			grid->setColumnStretch(c, 1);
		for (int r = 0; r < rows; ++r)
			grid->setRowStretch(r, 1);
		mainLayout->addLayout(grid, 1);
	}

	m_hintLabel = new QLabel(this);
	m_hintLabel->setAlignment(Qt::AlignCenter);
	mainLayout->addWidget(m_hintLabel, 0);
	updateHintText();

	if (!restoreWindowGeometry(this, "photoCompareWindow"))
	{
		resize(1200, 800);  // the size the window falls back to when un-maximized
		setWindowState(Qt::WindowMaximized);  // "use the whole screen" by default; the saved geometry rules thereafter
	}
}

PhotoCompareWindow::~PhotoCompareWindow()
{
	saveWindowGeometry(this, "photoCompareWindow");
}

void PhotoCompareWindow::keyPressEvent(QKeyEvent* event)
{
	const int key = event->key();
	if (key == Qt::Key_Escape)
	{
		if (m_calibrating)
			setCalibrating(false);
		else
			close();
	}
	else if (key == Qt::Key_A && !event->isAutoRepeat() && !m_panes.empty())
		setCalibrating(!m_calibrating);
	else if (key == Qt::Key_F)
		fitView();
	else if (!m_calibrating && !event->isAutoRepeat() &&
	         key >= Qt::Key_1 && key < Qt::Key_1 + static_cast<int>(m_panes.size()))
	{
		m_flickerIndex = key - Qt::Key_1;
		updateAllPanes();
	}
	else
		QWidget::keyPressEvent(event);
}

void PhotoCompareWindow::keyReleaseEvent(QKeyEvent* event)
{
	if (!event->isAutoRepeat() && m_flickerIndex >= 0 && event->key() == Qt::Key_1 + m_flickerIndex)
	{
		m_flickerIndex = -1;
		updateAllPanes();
	}
	else
		QWidget::keyReleaseEvent(event);
}

QPointF PhotoCompareWindow::subjectFromWidget(const QPointF& widgetPos) const
{
	return (widgetPos - m_viewPan) / m_viewZoom;
}

QPointF PhotoCompareWindow::widgetFromImage(const Pane& pane, const QPointF& imagePos) const
{
	return m_viewZoom * (pane.alignScale * imagePos + pane.alignOffset) + m_viewPan;
}

QPointF PhotoCompareWindow::imageFromWidget(const Pane& pane, const QPointF& widgetPos) const
{
	return (subjectFromWidget(widgetPos) - pane.alignOffset) / pane.alignScale;
}

const QImage& PhotoCompareWindow::imageForScale(Pane& pane, double effectiveScale, double& residualScale)
{
	// Pick the halving-chain level whose scale is the smallest one still >= effectiveScale, so the painter's
	// live bilinear pass only ever minifies by <= 2x - bilinear aliases badly at larger reductions (the same
	// lesson as ThumbnailWidget's pre-resample fix). Levels are built once, on demand.
	int level = 0;
	double levelScale = 1.0;
	while (levelScale * 0.5 >= effectiveScale && level < 16)
	{
		levelScale *= 0.5;
		++level;
	}
	while (static_cast<int>(pane.mipmaps.size()) < level)
	{
		const QImage& prev = pane.mipmaps.empty() ? pane.image : pane.mipmaps.back();
		if (prev.width() <= 1 || prev.height() <= 1)
			break;  // can't halve any further; level is clamped to what exists below
		pane.mipmaps.push_back(prev.scaled((prev.width() + 1) / 2, (prev.height() + 1) / 2,
		                                   Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
	}
	level = std::min(level, static_cast<int>(pane.mipmaps.size()));
	residualScale = effectiveScale * std::pow(2.0, level);
	return level == 0 ? pane.image : pane.mipmaps[level - 1];
}

void PhotoCompareWindow::zoomView(double factor, const QPointF& widgetAnchor)
{
	const double newZoom = std::clamp(m_viewZoom * factor, 0.01, 100.0);
	const double applied = newZoom / m_viewZoom;
	m_viewZoom = newZoom;
	m_viewPan = widgetAnchor - applied * (widgetAnchor - m_viewPan);  // keeps the subject point under the cursor fixed
	m_viewTouched = true;
	updateAllPanes();
}

void PhotoCompareWindow::panView(const QPointF& widgetDelta)
{
	m_viewPan += widgetDelta;
	m_viewTouched = true;
	updateAllPanes();
}

void PhotoCompareWindow::fitView()
{
	if (m_panes.empty())
		return;
	const QSizeF paneSize = m_paneWidgets[0]->size();
	if (paneSize.isEmpty())
		return;
	const Pane& ref = m_panes[m_refIndex];
	const QRectF subjectRect(ref.alignOffset, QSizeF(ref.image.size()) * ref.alignScale);
	m_viewZoom = std::min(paneSize.width() / subjectRect.width(), paneSize.height() / subjectRect.height());
	m_viewPan = QPointF((paneSize.width() - m_viewZoom * subjectRect.width()) / 2.0,
	                    (paneSize.height() - m_viewZoom * subjectRect.height()) / 2.0)
	            - m_viewZoom * subjectRect.topLeft();
	updateAllPanes();
}

void PhotoCompareWindow::adjustPaneScale(int index, double factor, const QPointF& widgetAnchor)
{
	// Rescale one photo's alignment keeping ITS point under the cursor fixed (the other panes don't move).
	Pane& pane = m_panes[index];
	const QPointF subjectAnchor = subjectFromWidget(widgetAnchor);
	const QPointF imageAnchor = (subjectAnchor - pane.alignOffset) / pane.alignScale;
	pane.alignScale = std::clamp(pane.alignScale * factor, 0.01, 100.0);
	pane.alignOffset = subjectAnchor - pane.alignScale * imageAnchor;
	m_viewTouched = true;
	updateAllPanes();  // flicker can be rendering this photo in other panes, so update all (it's <= 4 repaints)
}

void PhotoCompareWindow::movePaneOffset(int index, const QPointF& widgetDelta)
{
	m_panes[index].alignOffset += widgetDelta / m_viewZoom;
	m_viewTouched = true;
	updateAllPanes();
}

Qt::CursorShape PhotoCompareWindow::idleCursor() const
{
	return m_calibrating ? Qt::CrossCursor : Qt::OpenHandCursor;
}

void PhotoCompareWindow::setCalibrating(bool calibrating)
{
	m_calibrating = calibrating;
	// Calibration clicks map to the pane's own photo, so a still-held flicker override (panes showing some
	// other photo) would have the user placing points against the wrong picture - drop it.
	m_flickerIndex = -1;
	for (Pane& pane : m_panes)
		pane.calibPoints.clear();
	for (PhotoComparePane* paneWidget : m_paneWidgets)
		paneWidget->setCursor(idleCursor());
	updateHintText();
	updateAllPanes();
}

void PhotoCompareWindow::addCalibrationPoint(int index, const QPointF& imagePos)
{
	auto& points = m_panes[index].calibPoints;
	if (points.size() >= 2)
		return;
	// A near-duplicate would make the two-point distance ratio meaningless (or divide by ~zero) - ignore it.
	if (points.size() == 1 && QLineF(points[0], imagePos).length() < 4.0)
		return;
	// The pane receiving the session's very first point becomes the reference the others are mapped onto.
	if (std::all_of(m_panes.cbegin(), m_panes.cend(), [](const Pane& pane) { return pane.calibPoints.empty(); }))
		m_refIndex = index;
	points.push_back(imagePos);
	updateHintText();
	m_paneWidgets[index]->update();

	const bool allPlaced = std::all_of(m_panes.cbegin(), m_panes.cend(),
	                                   [](const Pane& pane) { return pane.calibPoints.size() == 2; });
	if (allPlaced)
		applyCalibration();
}

void PhotoCompareWindow::undoCalibrationPoint(int index)
{
	auto& points = m_panes[index].calibPoints;
	if (points.empty())
		return;
	points.pop_back();
	updateHintText();
	m_paneWidgets[index]->update();
}

void PhotoCompareWindow::applyCalibration()
{
	// The reference is the pane that received the session's first point: subject space becomes its pixel
	// coords. Every other photo maps onto it by the transform (uniform scale + offset - no rotation in v1)
	// that carries its two clicked points onto the reference's two: scale = the distance ratio, offset =
	// the midpoint difference.
	Pane& ref = m_panes[m_refIndex];
	const QLineF refLine(ref.calibPoints[0], ref.calibPoints[1]);
	// Fold the reference's old alignment into the view transform so the reference image stays exactly where
	// it was on screen (same zoom, same position) and only the other photos move to meet it.
	m_viewPan += m_viewZoom * ref.alignOffset;
	m_viewZoom *= ref.alignScale;
	ref.alignScale = 1.0;
	ref.alignOffset = QPointF();
	for (size_t i = 0; i < m_panes.size(); ++i)
	{
		if (static_cast<int>(i) == m_refIndex)
			continue;
		Pane& pane = m_panes[i];
		const QLineF line(pane.calibPoints[0], pane.calibPoints[1]);
		pane.alignScale = refLine.length() / line.length();
		pane.alignOffset = refLine.center() - pane.alignScale * line.center();
	}
	setCalibrating(false);  // also repaints all panes
}

void PhotoCompareWindow::onPaneResized()
{
	// Until the user navigates, every layout change (first show, maximize, a later window resize) re-fits
	// the view; after that the view is theirs and resizes leave it alone.
	if (!m_viewTouched)
		fitView();
}

void PhotoCompareWindow::updateAllPanes()
{
	for (PhotoComparePane* paneWidget : m_paneWidgets)
		paneWidget->update();
}

void PhotoCompareWindow::updateHintText()
{
	if (m_panes.empty())
		m_hintLabel->setText(tr("Esc: close"));
	else if (m_calibrating)
	{
		QStringList progress;
		for (size_t i = 0; i < m_panes.size(); ++i)
		{
			QString entry = QString("%1: %2/2").arg(i + 1).arg(m_panes[i].calibPoints.size());
			if (static_cast<int>(i) == m_refIndex && !m_panes[i].calibPoints.empty())
				entry += tr(" (ref)");
			progress.push_back(entry);
		}
		m_hintLabel->setText(tr("Alignment: click the same two features in every photo · right-click: undo a point · Esc: cancel · %1")
			.arg(progress.join("   ")));
	}
	else
		m_hintLabel->setText(tr("Wheel: zoom · drag: pan · Ctrl+wheel / Ctrl+drag: adjust one photo · A: align by 2 points · hold 1..%1: flicker · F / double-click: fit · Esc: close")
			.arg(m_panes.size()));
}
