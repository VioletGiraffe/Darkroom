#include "Windows/PhotoCompareWindow.h"
#include "MagicAlignment.h"
#include "Theme/Theme.h"
#include "UiComponents/SegmentedToggle.h"
#include "Utils.h"

#include <QApplication>
#include <QDebug>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QImageReader>
#include <QKeyEvent>
#include <QLabel>
#include <QLineF>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QSlider>
#include <QStackedLayout>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace {

constexpr double Pi = 3.14159265358979323846;

// p rotated by angle (radians) about the origin - the R factor of a photo's image -> subject similarity.
QPointF rotated(const QPointF& p, double angle)
{
	const double c = std::cos(angle), s = std::sin(angle);
	return QPointF(c * p.x() - s * p.y(), s * p.x() + c * p.y());
}

} // namespace

// One grid cell: a viewport onto the shared view. All state (images, alignment, the view itself) lives in
// the owning PhotoCompareWindow; the pane renders and translates mouse input into owner calls.
// The same class also serves as the full-view pane (index -1), which shows whatever photo the owner's
// m_fullViewIndex selects - photoIndex() resolves that indirection.
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
	// The photo this pane represents: its own grid position, or the slider-picked one for the full-view pane.
	[[nodiscard]] int photoIndex() const { return m_index >= 0 ? m_index : m_owner.m_fullViewIndex; }

	PhotoCompareWindow& m_owner;
	const int m_index;  // grid/photo index, or -1 for the full-view pane

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

	const int renderIndex = m_owner.m_flickerIndex >= 0 ? m_owner.m_flickerIndex : photoIndex();
	PhotoCompareWindow::Photo& photo = m_owner.m_photos[renderIndex];
	const double effectiveScale = m_owner.m_viewZoom * photo.alignScale;

	const auto drawPhoto = [&](PhotoCompareWindow::Photo& drawn) {
		const double drawnScale = m_owner.m_viewZoom * drawn.alignScale;
		double residualScale = 1.0;
		const QImage& source = m_owner.imageForScale(drawn, drawnScale, residualScale);
		painter.save();
		painter.setRenderHint(QPainter::SmoothPixmapTransform);
		painter.translate(m_owner.m_viewZoom * drawn.alignOffset + m_owner.m_viewPan);
		painter.rotate(drawn.alignRotation * (180.0 / Pi));  // rotation and uniform scale commute, so the order is free
		painter.scale(residualScale, residualScale);
		painter.drawImage(0, 0, source);
		painter.restore();
	};

	// Difference mode renders every photo except the reference as its per-channel |photo - reference|:
	// the reference is drawn first, the photo on top of it in Difference composition mode. Where only one
	// of the two covers, that image differences against the matte, i.e. shows (nearly) unchanged.
	if (m_owner.m_differenceMode && renderIndex != m_owner.m_refIndex)
	{
		drawPhoto(m_owner.m_photos[m_owner.m_refIndex]);
		painter.save();
		painter.setCompositionMode(QPainter::CompositionMode_Difference);
		drawPhoto(photo);
		painter.restore();
	}
	else
		drawPhoto(photo);

	painter.setRenderHint(QPainter::Antialiasing);

	// Calibration crosshairs - always the pane's OWN points (flicker is inert while calibrating).
	if (m_owner.m_calibrating)
	{
		painter.setPen(QPen(QColor(Theme::current().AccentBorder), 2));
		for (const QPointF& imagePos : m_owner.m_photos[photoIndex()].calibPoints)
		{
			const QPointF c = m_owner.widgetFromImage(m_owner.m_photos[photoIndex()], imagePos);
			painter.drawLine(c - QPointF(8, 0), c + QPointF(8, 0));
			painter.drawLine(c - QPointF(0, 8), c + QPointF(0, 8));
		}
	}

	// Auto-align diagnostics: where the aligner took its evidence in the RENDERED photo, drawn at the
	// patch's true on-screen footprint. Accent = used for the fit; orange = matched well but inconsistent
	// with the fitted transform (outlier - locally moved content, parallax, ...); red = failed to match.
	painter.setBrush(Qt::NoBrush);
	const double markHalf = 0.5 * m_owner.m_alignMarkSize * m_owner.m_viewZoom;
	for (const PhotoCompareWindow::AlignmentMark& mark : photo.alignMarks)
	{
		const QColor color = mark.kind == PhotoCompareWindow::AlignmentMark::Kind::Used ? QColor(Theme::current().AccentBorder)
		                   : mark.kind == PhotoCompareWindow::AlignmentMark::Kind::Outlier ? QColor(0xe0, 0xa2, 0x30)
		                                                                                   : QColor(0xd0, 0x40, 0x40);
		painter.setPen(QPen(color, 2));
		const QPointF center = m_owner.widgetFromImage(photo, mark.imagePos);
		painter.drawRect(QRectF(center.x() - markHalf, center.y() - markHalf, 2.0 * markHalf, 2.0 * markHalf));
	}

	// Corner caption, two lines. Headline "2 · name.jpg · 63%": the leading digit doubles as the pane's
	// flicker key, the percentage is this photo's on-screen scale (100% = 1 image px per widget px), making
	// any compensation difference between panes visible at a glance. Second line spells out this photo's raw
	// alignment similarity (image -> subject scale, rotation in degrees, offset in subject px) so every
	// parameter driving the overlay is inspectable per pane.
	const QString headline = QString("%1 · %2 · %3%")
		.arg(renderIndex + 1).arg(photo.caption).arg(qRound(effectiveScale * 100.0));
	const QString alignLine = QString("scale %1 · rot %2° · offset (%3, %4)")
		.arg(photo.alignScale, 0, 'f', 3)
		.arg(photo.alignRotation * (180.0 / Pi), 0, 'f', 2)
		.arg(qRound(photo.alignOffset.x())).arg(qRound(photo.alignOffset.y()));
	const QFontMetrics fm = painter.fontMetrics();
	const int textWidth = std::max(fm.horizontalAdvance(headline), fm.horizontalAdvance(alignLine));
	const QRectF textRect(8, 8, textWidth + 12, 2 * fm.height() + 6);
	painter.setPen(Qt::NoPen);
	painter.setBrush(QColor(0, 0, 0, 150));
	painter.drawRoundedRect(textRect, 4, 4);
	painter.setPen(QColor(0xe8, 0xe8, 0xe8));
	painter.drawText(textRect, Qt::AlignCenter, headline + "\n" + alignLine);

	// A flickered pane shows a photo other than its own - flag it with an accent frame.
	if (renderIndex != photoIndex())
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
		m_owner.adjustPhotoScale(photoIndex(), factor, event->position());
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
		m_owner.undoCalibrationPoint(photoIndex());
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
			m_owner.movePhotoOffset(photoIndex(), pos - m_lastDragPos);
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
		m_owner.addCalibrationPoint(photoIndex(), m_owner.imageFromWidget(m_owner.m_photos[photoIndex()], m_pressPos));
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
	setAcceptDrops(true);  // no child accepts drops, so a drop anywhere in the window lands here

	QVBoxLayout* mainLayout = new QVBoxLayout(this);
	mainLayout->setContentsMargins(4, 4, 4, 4);

	m_gridPage = new QWidget(this);
	m_dropHintLabel = new QLabel(tr("Drop image files here to compare them"), m_gridPage);
	m_dropHintLabel->setAlignment(Qt::AlignCenter);

	m_fullPane = new PhotoComparePane(*this, -1);
	m_viewStack = new QStackedLayout();
	m_viewStack->addWidget(m_gridPage);
	m_viewStack->addWidget(m_fullPane);
	mainLayout->addLayout(m_viewStack, 1);

	// Bottom toolbar: the full-view picker slider stretching across, render-mode toggle at the far right.
	QHBoxLayout* toolbar = new QHBoxLayout();

	// Full-view picker: one detent per photo; pressing the handle or changing the value enters the full
	// view, dragging back and forth scrubs between the aligned photos (a flicker gesture at full size).
	m_slider = new QSlider(Qt::Horizontal, this);
	m_slider->setPageStep(1);
	m_slider->setTickPosition(QSlider::TicksBelow);
	m_slider->setTickInterval(1);
	m_slider->setFocusPolicy(Qt::NoFocus);  // all keyboard input stays on the window
	connect(m_slider, &QSlider::sliderPressed, this, [this] { setFullViewIndex(m_slider->value()); });
	connect(m_slider, &QSlider::valueChanged, this, [this](int value) { setFullViewIndex(value); });
	toolbar->addWidget(m_slider, 1);

	m_diffToggle = new SegmentedToggle({ tr("Normal"), tr("Difference") }, this);
	m_diffToggle->setToolTip(tr("Difference: render each photo as its per-pixel difference against the reference photo (D)"));
	connect(m_diffToggle, &SegmentedToggle::currentChanged, this, [this](int index) { setDifferenceMode(index == 1); });
	toolbar->addWidget(m_diffToggle, 0);

	mainLayout->addLayout(toolbar, 0);

	m_hintLabel = new QLabel(this);
	m_hintLabel->setAlignment(Qt::AlignCenter);
	mainLayout->addWidget(m_hintLabel, 0);

	addPhotosFromFiles(photoPaths);  // also puts the empty drop-target state in place when the list is empty

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

void PhotoCompareWindow::addPhotosFromFiles(const QStringList& photoPaths)
{
	const size_t oldCount = m_photos.size();
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
		Photo photo;
		photo.image = std::move(image);
		photo.caption = QFileInfo(path).fileName();
		// Default alignment: normalize the photo's height to the reference's subject-space height and center
		// the two on each other - so identical shots that only differ in export resolution line up with no
		// user action at all. The very first photo keeps the identity default: it defines subject space.
		if (!m_photos.empty())
		{
			const Photo& ref = m_photos[m_refIndex];
			const QRectF refRect(ref.alignOffset, QSizeF(ref.image.size()) * ref.alignScale);
			photo.alignScale = refRect.height() / photo.image.height();
			photo.alignOffset = refRect.center() - photo.alignScale * QPointF(photo.image.width(), photo.image.height()) / 2.0;
		}
		m_photos.push_back(std::move(photo));
	}

	rebuildPaneGrid();
	m_slider->setRange(0, std::max(0, static_cast<int>(m_photos.size()) - 1));
	m_slider->setEnabled(!m_photos.empty() && !m_calibrating);
	if (!m_viewTouched)
		fitView();
	updateHintText();
	updateAllPanes();
	if (m_photos.size() == oldCount && !photoPaths.isEmpty())  // transient status, like the auto-align summary
		m_hintLabel->setText(tr("None of the files could be loaded as images."));
}

void PhotoCompareWindow::rebuildPaneGrid()
{
	// Rebuilt from scratch on every photo count change: recreate the layout (panes stay children of the page)
	// and re-place every pane - a fresh layout also drops the previous geometry's row/column stretches.
	delete m_gridPage->layout();
	QGridLayout* grid = new QGridLayout(m_gridPage);
	grid->setContentsMargins(0, 0, 0, 0);
	grid->setSpacing(4);

	const int photoCount = static_cast<int>(m_photos.size());
	m_dropHintLabel->setVisible(photoCount == 0);
	if (photoCount == 0)
	{
		grid->addWidget(m_dropHintLabel, 0, 0);
		return;
	}

	while (static_cast<int>(m_paneWidgets.size()) < photoCount)
		m_paneWidgets.push_back(new PhotoComparePane(*this, static_cast<int>(m_paneWidgets.size())));

	const int columns = static_cast<int>(std::ceil(std::sqrt(photoCount)));
	const int rows = (photoCount + columns - 1) / columns;
	for (int i = 0; i < photoCount; ++i)
		grid->addWidget(m_paneWidgets[i], i / columns, i % columns);
	// Equal stretch keeps every cell the same size (including a 3-way compare's empty 4th cell): the
	// shared pan is in widget coordinates, so equally sized panes is what makes the same widget position
	// show the same subject point in each.
	for (int c = 0; c < columns; ++c)
		grid->setColumnStretch(c, 1);
	for (int r = 0; r < rows; ++r)
		grid->setRowStretch(r, 1);
}

void PhotoCompareWindow::dragEnterEvent(QDragEnterEvent* event)
{
	// Accept any local files; whether they are images is decided by the load attempt on drop.
	const QList<QUrl> urls = event->mimeData()->urls();
	if (std::any_of(urls.cbegin(), urls.cend(), [](const QUrl& url) { return url.isLocalFile(); }))
		event->acceptProposedAction();
}

void PhotoCompareWindow::dropEvent(QDropEvent* event)
{
	QStringList paths;
	for (const QUrl& url : event->mimeData()->urls())
	{
		if (url.isLocalFile())
			paths.push_back(url.toLocalFile());
	}
	addPhotosFromFiles(paths);
	event->acceptProposedAction();
}

void PhotoCompareWindow::keyPressEvent(QKeyEvent* event)
{
	const int key = event->key();
	if (key == Qt::Key_Escape)
	{
		if (m_fullViewIndex >= 0)
			exitFullView();
		else if (m_calibrating)
			setCalibrating(false);
		else
			close();
	}
	else if (key == Qt::Key_A && !event->isAutoRepeat() && !m_photos.empty())
	{
		if (event->modifiers().testFlag(Qt::ShiftModifier))
		{
			exitFullView();  // calibration points are placed per grid pane
			setCalibrating(!m_calibrating);
		}
		else
			autoAlignPhotos();  // works in the full view too
	}
	else if (key == Qt::Key_F)
		fitView();
	else if (key == Qt::Key_D && !event->isAutoRepeat() && !m_photos.empty())
		setDifferenceMode(!m_differenceMode);
	else if (m_fullViewIndex >= 0 && (key == Qt::Key_Left || key == Qt::Key_Right))
		m_slider->setValue(m_slider->value() + (key == Qt::Key_Right ? 1 : -1));  // setValue clamps to the range
	else if (!m_calibrating && !event->isAutoRepeat() &&
	         key >= Qt::Key_1 && key <= Qt::Key_9 && key < Qt::Key_1 + static_cast<int>(m_photos.size()))
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

QPointF PhotoCompareWindow::widgetFromImage(const Photo& photo, const QPointF& imagePos) const
{
	return m_viewZoom * (photo.alignScale * rotated(imagePos, photo.alignRotation) + photo.alignOffset) + m_viewPan;
}

QPointF PhotoCompareWindow::imageFromWidget(const Photo& photo, const QPointF& widgetPos) const
{
	return rotated((subjectFromWidget(widgetPos) - photo.alignOffset) / photo.alignScale, -photo.alignRotation);
}

const QImage& PhotoCompareWindow::imageForScale(Photo& photo, double effectiveScale, double& residualScale)
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
	while (static_cast<int>(photo.mipmaps.size()) < level)
	{
		const QImage& prev = photo.mipmaps.empty() ? photo.image : photo.mipmaps.back();
		if (prev.width() <= 1 || prev.height() <= 1)
			break;  // can't halve any further; level is clamped to what exists below
		photo.mipmaps.push_back(prev.scaled((prev.width() + 1) / 2, (prev.height() + 1) / 2,
		                                    Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
	}
	level = std::min(level, static_cast<int>(photo.mipmaps.size()));
	residualScale = effectiveScale * std::pow(2.0, level);
	return level == 0 ? photo.image : photo.mipmaps[level - 1];
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
	if (m_photos.empty())
		return;
	const QSizeF paneSize = (m_fullViewIndex >= 0 ? m_fullPane : m_paneWidgets[0])->size();
	if (paneSize.isEmpty())
		return;
	const Photo& ref = m_photos[m_refIndex];
	const QRectF subjectRect(ref.alignOffset, QSizeF(ref.image.size()) * ref.alignScale);
	m_viewZoom = std::min(paneSize.width() / subjectRect.width(), paneSize.height() / subjectRect.height());
	m_viewPan = QPointF((paneSize.width() - m_viewZoom * subjectRect.width()) / 2.0,
	                    (paneSize.height() - m_viewZoom * subjectRect.height()) / 2.0)
	            - m_viewZoom * subjectRect.topLeft();
	updateAllPanes();
}

void PhotoCompareWindow::adjustPhotoScale(int index, double factor, const QPointF& widgetAnchor)
{
	// Rescale one photo's alignment keeping ITS point under the cursor fixed (the other panes don't move).
	Photo& photo = m_photos[index];
	const QPointF subjectAnchor = subjectFromWidget(widgetAnchor);
	const QPointF imageAnchor = imageFromWidget(photo, widgetAnchor);
	photo.alignScale = std::clamp(photo.alignScale * factor, 0.01, 100.0);
	photo.alignOffset = subjectAnchor - photo.alignScale * rotated(imageAnchor, photo.alignRotation);
	m_viewTouched = true;
	updateAllPanes();  // flicker can be rendering this photo in other panes, so update all (it's <= 4 repaints)
}

void PhotoCompareWindow::movePhotoOffset(int index, const QPointF& widgetDelta)
{
	m_photos[index].alignOffset += widgetDelta / m_viewZoom;
	m_viewTouched = true;
	updateAllPanes();
}

void PhotoCompareWindow::autoAlignPhotos()
{
	if (m_photos.size() < 2)
		return;
	if (m_calibrating)
		setCalibrating(false);
	QApplication::setOverrideCursor(Qt::BusyCursor);
	for (Photo& photo : m_photos)
		photo.alignMarks.clear();

	Photo& ref = m_photos[m_refIndex];
	// The library works in the reference's PIXEL space; subject space differs from it by the reference's own
	// transform. Fold that into the view first (keeps the reference pixel-frozen on screen, same as manual
	// calibration) - from here on subject space IS reference pixel space, and each photo's current mapping
	// into it serves both as the initial guess and as the kept alignment when the aligner reports failure.
	// The view has no rotation, so only the scale+offset part can be folded: a (rare) rotation on the
	// reference itself becomes a small one-time visual jump as it rebases; subject space stays exact.
	const double refScale = ref.alignScale;
	const double refRotation = ref.alignRotation;
	const QPointF refOffset = ref.alignOffset;
	m_viewPan += m_viewZoom * refOffset;
	m_viewZoom *= refScale;
	ref.alignScale = 1.0;
	ref.alignRotation = 0.0;
	ref.alignOffset = QPointF();

	QStringList summary;
	for (size_t i = 0; i < m_photos.size(); ++i)
	{
		if (static_cast<int>(i) == m_refIndex)
			continue;
		Photo& photo = m_photos[i];
		AlignmentOptions options;
		// refTransform^-1 * photoTransform: the photo's mapping into the rebased subject space.
		options.initialGuess = { photo.alignScale / refScale, photo.alignRotation - refRotation,
		                         rotated(photo.alignOffset - refOffset, -refRotation) / refScale };
		const AlignmentResult result = alignImages(ref.image, photo.image, options);
		m_alignMarkSize = result.patchSize;  // in reference px == subject units (the subject space was just rebased)
		qDebug() << "Auto-align photo" << (i + 1) << "vs" << (m_refIndex + 1) << ": succeeded" << result.succeeded
		         << " confidence" << result.confidence << " coarse" << result.bootstrapZncc
		         << " scale" << result.transform.scale << " offset" << result.transform.offset
		         << " rotation" << result.transform.rotation * (180.0 / Pi) << "deg";
		static constexpr const char* fateNames[] = { "accepted", "outside overlap", "flat", "weak match", "outlier" };
		for (const AlignmentPatchInfo& patchInfo : result.patches)
		{
			qDebug() << "  patch @" << patchInfo.refPoint << fateNames[static_cast<int>(patchInfo.fate)]
			         << " zncc" << patchInfo.zncc << " residual" << patchInfo.residual;
			const auto kind = patchInfo.fate == AlignmentPatchFate::Accepted ? AlignmentMark::Kind::Used
			                : patchInfo.fate == AlignmentPatchFate::Outlier  ? AlignmentMark::Kind::Outlier
			                                                                 : AlignmentMark::Kind::Failed;
			ref.alignMarks.push_back({ patchInfo.refPoint, kind });
			if (patchInfo.zncc > 0.0)  // targetPoint is meaningless for a patch that never matched
				photo.alignMarks.push_back({ patchInfo.targetPoint, kind });
		}
		if (result.succeeded)
		{
			photo.alignScale = result.transform.scale;
			photo.alignRotation = result.transform.rotation;
			photo.alignOffset = result.transform.offset;
			QString entry = tr("%1: aligned (%2)").arg(i + 1).arg(result.confidence, 0, 'f', 2);
			// Rotation is corrected like the rest of the transform, but it is the one component with no
			// manual-adjustment gesture - surface notable angles.
			const double rotationDegrees = result.transform.rotation * (180.0 / Pi);
			if (std::abs(rotationDegrees) >= 0.05)
				entry += tr(" rot %1°").arg(rotationDegrees, 0, 'f', 2);
			summary << entry;
		}
		else
		{
			photo.alignScale = options.initialGuess.scale;
			photo.alignRotation = options.initialGuess.rotation;
			photo.alignOffset = options.initialGuess.offset;
			summary << tr("%1: FAILED, kept (conf %2, coarse %3)")
				.arg(i + 1).arg(result.confidence, 0, 'f', 2).arg(result.bootstrapZncc, 0, 'f', 2);
		}
	}
	QApplication::restoreOverrideCursor();
	// Transient status in the hint bar; any later mode change repaints the normal hint over it.
	m_hintLabel->setText(tr("Auto-align vs %1 · %2 · squares: accent = used, orange = outlier, red = no match")
		.arg(m_refIndex + 1).arg(summary.join("   ")));
	updateAllPanes();
}

Qt::CursorShape PhotoCompareWindow::idleCursor() const
{
	return m_calibrating ? Qt::CrossCursor : Qt::OpenHandCursor;
}

void PhotoCompareWindow::setCalibrating(bool calibrating)
{
	m_calibrating = calibrating;
	m_slider->setEnabled(!calibrating);  // the full view has no per-pane clicks, so it is off-limits mid-calibration
	// Calibration clicks map to the pane's own photo, so a still-held flicker override (panes showing some
	// other photo) would have the user placing points against the wrong picture - drop it.
	m_flickerIndex = -1;
	for (Photo& photo : m_photos)
	{
		photo.calibPoints.clear();
		photo.alignMarks.clear();  // stale auto-align circles would only be clutter under the crosshairs
	}
	for (PhotoComparePane* paneWidget : m_paneWidgets)
		paneWidget->setCursor(idleCursor());
	updateHintText();
	updateAllPanes();
}

void PhotoCompareWindow::addCalibrationPoint(int index, const QPointF& imagePos)
{
	auto& points = m_photos[index].calibPoints;
	if (points.size() >= 2)
		return;
	// A near-duplicate would make the two-point distance ratio meaningless (or divide by ~zero) - ignore it.
	if (points.size() == 1 && QLineF(points[0], imagePos).length() < 4.0)
		return;
	// The pane receiving the session's very first point becomes the reference the others are mapped onto.
	if (std::all_of(m_photos.cbegin(), m_photos.cend(), [](const Photo& photo) { return photo.calibPoints.empty(); }))
		m_refIndex = index;
	points.push_back(imagePos);
	updateHintText();
	m_paneWidgets[index]->update();

	const bool allPlaced = std::all_of(m_photos.cbegin(), m_photos.cend(),
	                                   [](const Photo& photo) { return photo.calibPoints.size() == 2; });
	if (allPlaced)
		applyCalibration();
}

void PhotoCompareWindow::undoCalibrationPoint(int index)
{
	auto& points = m_photos[index].calibPoints;
	if (points.empty())
		return;
	points.pop_back();
	updateHintText();
	m_paneWidgets[index]->update();
}

void PhotoCompareWindow::applyCalibration()
{
	// The reference is the pane that received the session's first point: subject space becomes its pixel
	// coords. Every other photo maps onto it by the similarity that carries its two clicked points exactly
	// onto the reference's two: scale = the distance ratio, rotation = the angle between the segments,
	// offset = what maps the midpoints. Two point pairs determine all four parameters, so this handles
	// arbitrary angles - beyond auto-align's small-angle capture range.
	Photo& ref = m_photos[m_refIndex];
	const QLineF refLine(ref.calibPoints[0], ref.calibPoints[1]);
	// Fold the reference's old alignment into the view transform so the reference image stays exactly where
	// it was on screen (same zoom, same position) and only the other photos move to meet it. The view has no
	// rotation, so a (rare) rotation on the reference itself becomes a small one-time visual jump.
	m_viewPan += m_viewZoom * ref.alignOffset;
	m_viewZoom *= ref.alignScale;
	ref.alignScale = 1.0;
	ref.alignRotation = 0.0;
	ref.alignOffset = QPointF();
	for (size_t i = 0; i < m_photos.size(); ++i)
	{
		if (static_cast<int>(i) == m_refIndex)
			continue;
		Photo& photo = m_photos[i];
		const QLineF line(photo.calibPoints[0], photo.calibPoints[1]);
		photo.alignScale = refLine.length() / line.length();
		double rotation = std::atan2(refLine.dy(), refLine.dx()) - std::atan2(line.dy(), line.dx());
		if (rotation > Pi)  // wrap the atan2 difference back into (-Pi, Pi]
			rotation -= 2.0 * Pi;
		else if (rotation <= -Pi)
			rotation += 2.0 * Pi;
		photo.alignRotation = rotation;
		photo.alignOffset = refLine.center() - photo.alignScale * rotated(line.center(), rotation);
	}
	setCalibrating(false);  // also repaints all panes
}

void PhotoCompareWindow::setFullViewIndex(int index)
{
	const bool entering = m_fullViewIndex < 0;
	m_fullViewIndex = index;
	m_slider->setValue(index);  // no-op when the slider itself is the source
	if (entering)
	{
		// The viewport grows from one grid cell to the whole stack area. A touched view keeps the subject
		// point at the viewport's center fixed (both sizes are current when read here, before the switch);
		// an untouched one re-fits to the new viewport as it would on any resize.
		const QSizeF oldSize = m_paneWidgets[0]->size();
		const QSizeF newSize = m_viewStack->widget(0)->size();
		m_viewStack->setCurrentIndex(1);
		if (m_viewTouched)
			m_viewPan += QPointF(newSize.width() - oldSize.width(), newSize.height() - oldSize.height()) / 2.0;
		else
			fitView();  // a stale m_fullPane size here self-corrects: the layout resizes it -> onPaneResized -> re-fit
	}
	updateHintText();  // mode line, and the "N/M" position readout on every switch
	updateAllPanes();
}

void PhotoCompareWindow::exitFullView()
{
	if (m_fullViewIndex < 0)
		return;
	m_fullViewIndex = -1;
	const QSizeF oldSize = m_fullPane->size();
	const QSizeF newSize = m_paneWidgets[0]->size();  // grid panes keep their pre-full-view geometry while hidden
	m_viewStack->setCurrentIndex(0);
	if (m_viewTouched)
		m_viewPan += QPointF(newSize.width() - oldSize.width(), newSize.height() - oldSize.height()) / 2.0;
	else
		fitView();
	updateHintText();
	updateAllPanes();
}

void PhotoCompareWindow::setDifferenceMode(bool difference)
{
	m_differenceMode = difference;
	m_diffToggle->setCurrentIndex(difference ? 1 : 0);  // silent; a no-op when the toggle itself is the source
	updateAllPanes();
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
	if (m_fullPane)
		m_fullPane->update();
}

void PhotoCompareWindow::updateHintText()
{
	if (m_photos.empty())
		m_hintLabel->setText(tr("Drop image files onto the window to load them · Esc: close"));
	else if (m_calibrating)
	{
		QStringList progress;
		for (size_t i = 0; i < m_photos.size(); ++i)
		{
			QString entry = QString("%1: %2/2").arg(i + 1).arg(m_photos[i].calibPoints.size());
			if (static_cast<int>(i) == m_refIndex && !m_photos[i].calibPoints.empty())
				entry += tr(" (ref)");
			progress.push_back(entry);
		}
		m_hintLabel->setText(tr("Alignment: click the same two features in every photo · right-click: undo a point · Esc: cancel · %1")
			.arg(progress.join("   ")));
	}
	else if (m_fullViewIndex >= 0)
		m_hintLabel->setText(tr("Full view %1/%2 · slider / Left,Right: switch photo · hold 1..%2: flicker · A: auto-align · D: difference · wheel: zoom · drag: pan · Ctrl+wheel / Ctrl+drag: adjust this photo · F: fit · Esc: back to grid")
			.arg(m_fullViewIndex + 1).arg(m_photos.size()));
	else
		m_hintLabel->setText(tr("Wheel: zoom · drag: pan · Ctrl+wheel / Ctrl+drag: adjust one photo · A: auto-align · Shift+A: align by 2 clicks · hold 1..%1: flicker · D: difference · slider: full view · F / double-click: fit · Esc: close")
			.arg(m_photos.size()));
}
