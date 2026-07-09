#include "Windows/PhotoCompareWindow.h"
#include "MagicAlignment.h"
#include "Theme/Theme.h"
#include "UiComponents/SegmentedToggle.h"
#include "Utils.h"

#include <QApplication>
#include <QCheckBox>
#include <QContextMenuEvent>
#include <QDebug>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QImageReader>
#include <QKeyEvent>
#include <QLabel>
#include <QLineF>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QSettings>
#include <QSlider>
#include <QStackedLayout>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

// This window's persisted UI state. These keys live here, not in Settings.h, because they are local to the
// compare tool rather than app-wide configuration - but stay under the Settings namespace for the uniform
// QSettings{}.value(Settings::Foo) call style. The align region is stored as a QRectF of fractions of the
// reference frame (resolution-independent); an empty/absent value means no region.
namespace Settings {
	constexpr const char* PhotoCompareIgnoreRotation = "photoCompare/ignoreRotation";
	constexpr const char* PhotoCompareAoi = "photoCompare/aoiNormalized";
}

namespace {

constexpr double Pi = 3.14159265358979323846;

// p rotated by angle (radians) about the origin - the R factor of a photo's image -> subject similarity.
QPointF rotated(const QPointF& p, double angle)
{
	const double c = std::cos(angle), s = std::sin(angle);
	return QPointF(c * p.x() - s * p.y(), s * p.x() + c * p.y());
}

// Maps an axis-aligned subject-space rect through the INVERSE of an image -> subject similarity (scale,
// rotation, offset) into image coordinates - the bounding rect when rotation makes the image non-axis-aligned.
QRectF subjectRectToImage(const QRectF& rect, double scale, double rotation, const QPointF& offset)
{
	const QPointF corners[] = { rect.topLeft(), rect.topRight(), rect.bottomLeft(), rect.bottomRight() };
	QRectF result(rotated(corners[0] - offset, -rotation) / scale, QSizeF());
	for (int i = 1; i < 4; ++i)
	{
		const QPointF p = rotated(corners[i] - offset, -rotation) / scale;
		result.setLeft(std::min(result.left(), p.x()));
		result.setTop(std::min(result.top(), p.y()));
		result.setRight(std::max(result.right(), p.x()));
		result.setBottom(std::max(result.bottom(), p.y()));
	}
	return result;
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
	void contextMenuEvent(QContextMenuEvent* event) override;
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

	// Shift+drag rubber-bands the owner's auto-align region; the anchor is held in subject space so the
	// rect stays glued to the content even if the view is somehow moved mid-drag.
	bool m_aoiDrag = false;
	QPointF m_aoiAnchor;
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
	// patch's true on-screen footprint. Accent = used for the fit (dashed = used via a coarser-level match
	// only, e.g. defocused at full res); orange = matched well but inconsistent with the fitted transform
	// (outlier - locally moved content, parallax, ...); red = failed to match.
	painter.setBrush(Qt::NoBrush);
	const double markHalf = 0.5 * m_owner.m_alignMarkSize * m_owner.m_viewZoom;
	for (const PhotoCompareWindow::AlignmentMark& mark : photo.alignMarks)
	{
		using Kind = PhotoCompareWindow::AlignmentMark::Kind;
		const QColor color = mark.kind == Kind::Used || mark.kind == Kind::UsedCoarse ? QColor(Theme::current().AccentBorder)
		                   : mark.kind == Kind::Outlier ? QColor(0xe0, 0xa2, 0x30)
		                                                : QColor(0xd0, 0x40, 0x40);
		QPen pen(color, 2);
		if (mark.kind == Kind::UsedCoarse)
			pen.setStyle(Qt::DashLine);
		painter.setPen(pen);
		const QPointF center = m_owner.widgetFromImage(photo, mark.imagePos);
		painter.drawRect(QRectF(center.x() - markHalf, center.y() - markHalf, 2.0 * markHalf, 2.0 * markHalf));
	}

	// The auto-align region (Shift+drag): one subject-space rect, so it frames the same content in every
	// pane. Dashed light gray, to stay distinct from the accent patch marks.
	if (!m_owner.m_alignAoi.isEmpty())
	{
		painter.setPen(QPen(QColor(0xe8, 0xe8, 0xe8), 1, Qt::DashLine));
		painter.setBrush(Qt::NoBrush);
		painter.drawRect(QRectF(m_owner.m_viewZoom * m_owner.m_alignAoi.topLeft() + m_owner.m_viewPan,
		                        m_owner.m_alignAoi.size() * m_owner.m_viewZoom));
	}

	// Corner caption, stacked lines. Headline "2 · name.jpg · 6000x4000 (24 MP) · 63%": the leading digit
	// doubles as the pane's flicker key, then the photo's pixel resolution, then its on-screen scale (100% =
	// 1 image px per widget px), making any compensation difference between panes visible at a glance. The
	// alignment line spells out this photo's raw similarity into subject space (scale, rotation in degrees,
	// offset in subject px). The score
	// line appears only once auto-align has evaluated this photo: the run's two quality measures
	// (conf = weighted patch ZNCC fitness, coarse = coarse whole-frame score), the fitted rotation's 1-sigma
	// error bar, and the align call's runtime.
	QStringList captionLines;
	captionLines << QString("%1 · %2 · %3x%4 (%5 MP) · %6%")
		.arg(renderIndex + 1).arg(photo.caption)
		.arg(photo.image.width()).arg(photo.image.height())
		.arg(qRound(photo.image.width() * photo.image.height() / 1e6))
		.arg(qRound(effectiveScale * 100.0));
	captionLines << QString("scale %1 · rot %2° · offset (%3, %4)")
		.arg(photo.alignScale, 0, 'f', 3)
		.arg(photo.alignRotation * (180.0 / Pi), 0, 'f', 2)
		.arg(qRound(photo.alignOffset.x())).arg(qRound(photo.alignOffset.y()));
	if (photo.alignScored)
	{
		QString scoreLine = QString("conf %1 · coarse %2 · rot ±%3° · %4 ms")
			.arg(photo.alignConfidence, 0, 'f', 2).arg(photo.alignBootstrapZncc, 0, 'f', 2)
			.arg(photo.alignRotationSigma * (180.0 / Pi), 0, 'f', 2).arg(photo.alignTimeMs, 0, 'f', 0);
		if (!photo.alignSucceeded)  // the alignment on screen is the kept previous one - make that impossible to miss
			scoreLine.prepend(tr("FAILED · "));
		captionLines << scoreLine;
	}
	const QFontMetrics fm = painter.fontMetrics();
	int textWidth = 0;
	for (const QString& line : captionLines)
		textWidth = std::max(textWidth, fm.horizontalAdvance(line));
	const QRectF textRect(8, 8, textWidth + 12, captionLines.size() * fm.height() + 6.0);
	painter.setPen(Qt::NoPen);
	painter.setBrush(QColor(0, 0, 0, 150));
	painter.drawRoundedRect(textRect, 4, 4);
	painter.setPen(QColor(0xe8, 0xe8, 0xe8));
	painter.drawText(textRect, Qt::AlignCenter, captionLines.join('\n'));

	// The reference pane - the photo difference mode and both alignment paths work against - is outlined in yellow
	if (photoIndex() == m_owner.m_refIndex && m_owner.m_photos.size() > 1)
	{
		painter.setPen(QPen(QColor(240, 224, 64), 2.0));
		painter.setBrush(Qt::NoBrush);
		painter.drawRect(rect().adjusted(1, 1, -1, -1));
	}

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
	if (event->button() == Qt::LeftButton && event->modifiers().testFlag(Qt::ShiftModifier))
	{
		// Shift+drag marks the auto-align region; clearing on press makes a plain Shift+click the eraser.
		m_aoiDrag = true;
		m_aoiAnchor = m_owner.subjectFromWidget(event->position());
		m_owner.m_alignAoi = QRectF();
		m_owner.updateAllPanes();
		setCursor(Qt::CrossCursor);
	}
	else if (event->button() == Qt::LeftButton)
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
	if (m_aoiDrag)
	{
		m_owner.m_alignAoi = QRectF(m_aoiAnchor, m_owner.subjectFromWidget(event->position())).normalized();
		m_owner.updateAllPanes();
		return;
	}
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
	if (event->button() == Qt::LeftButton && m_aoiDrag)
	{
		m_aoiDrag = false;
		setCursor(m_owner.idleCursor());
		// A degenerate rect (a stray wiggle of a clearing Shift+click) is not a usable region - drop it.
		if (std::min(m_owner.m_alignAoi.width(), m_owner.m_alignAoi.height()) * m_owner.m_viewZoom < 8.0)
			m_owner.m_alignAoi = QRectF();
		m_owner.updateAllPanes();
		return;
	}
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

void PhotoComparePane::contextMenuEvent(QContextMenuEvent* event)
{
	if (m_owner.m_calibrating)  // right-click means "undo a point" while calibrating
		return;
	const int index = photoIndex();
	QMenu menu;
	menu.addAction(tr("Open containing folder"), [this, index] {
		if (const QString path = m_owner.m_photos[index].filePath; !openInExplorer(path))
			reportMissingFile(this, path);
	});
	QAction* makeReference = menu.addAction(tr("Make this the reference image"), [this, index] {
		m_owner.m_refIndex = index;
		m_owner.updateAllPanes();  // difference mode and the next align/calibration now work against this photo
	});
	makeReference->setEnabled(index != m_owner.m_refIndex);
	menu.exec(event->globalPos());
}

// ---------------------------------------------------------------------------------------------------------

void PhotoCompareWindow::showForFiles(const QStringList& candidatePaths, QWidget* parent)
{
	QStringList paths;
	static constexpr qsizetype MaxImages = 50;  // a bigger set stops being a useful comparison
	for (const QString& path : candidatePaths)
	{
		if (!path.isEmpty() && QFileInfo::exists(path))
		{
			paths.push_back(path);
			if (paths.size() >= MaxImages)
				break;
		}
	}

	if (paths.size() < 2)
	{
		QMessageBox::warning(parent, tr("Error"), tr("The selected photo files could not be found on disk."));
		return;
	}

	auto* w = new PhotoCompareWindow(paths, parent);
	w->setAttribute(Qt::WA_DeleteOnClose);
	w->show();
}

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

	// Bottom toolbar: the full-view picker slider stretching across; the align-option checkbox and the
	// render-mode toggle at the right.
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

	m_ignoreRotationCheck = new QCheckBox(tr("Ignore rotation"), this);
	m_ignoreRotationCheck->setToolTip(tr("Auto-align fits scale and offset only, treating any apparent rotation as spurious\n"
	                                     "(e.g. depth parallax between focus-stack slices can read as a slight tilt)"));
	m_ignoreRotationCheck->setFocusPolicy(Qt::NoFocus);  // all keyboard input stays on the window
	m_ignoreRotationCheck->setChecked(QSettings{}.value(Settings::PhotoCompareIgnoreRotation, false).toBool());
	connect(m_ignoreRotationCheck, &QCheckBox::toggled, this,
	        [](bool checked) { QSettings{}.setValue(Settings::PhotoCompareIgnoreRotation, checked); });
	toolbar->addWidget(m_ignoreRotationCheck, 0);

	m_diffToggle = new SegmentedToggle({ tr("Normal"), tr("Difference") }, this);
	m_diffToggle->setToolTip(tr("Difference: render each photo as its per-pixel difference against the reference photo (D)"));
	connect(m_diffToggle, &SegmentedToggle::currentChanged, this, [this](int index) { setDifferenceMode(index == 1); });
	toolbar->addWidget(m_diffToggle, 0);

	mainLayout->addLayout(toolbar, 0);

	m_hintLabel = new QLabel(this);
	m_hintLabel->setAlignment(Qt::AlignCenter);
	mainLayout->addWidget(m_hintLabel, 0);

	addPhotosFromFiles(photoPaths);  // also puts the empty drop-target state in place when the list is empty (the
	                                 // saved align region is restored inside, once the first photos actually exist)

	if (!restoreWindowGeometry(this, "photoCompareWindow"))
	{
		resize(1200, 800);  // the size the window falls back to when un-maximized
		setWindowState(Qt::WindowMaximized);  // "use the whole screen" by default; the saved geometry rules thereafter
	}
}

PhotoCompareWindow::~PhotoCompareWindow()
{
	saveWindowGeometry(this, "photoCompareWindow");
	// Persist the align region as fractions of the reference frame (resolution-independent). Empty (no region,
	// or no photos to anchor it) stores an empty rect, which reads back as "no region".
	QRectF normalizedAoi;
	if (!m_alignAoi.isEmpty() && !m_photos.empty())
	{
		const QRectF r = referenceSubjectRect();
		normalizedAoi = QRectF((m_alignAoi.x() - r.x()) / r.width(), (m_alignAoi.y() - r.y()) / r.height(),
		                       m_alignAoi.width() / r.width(), m_alignAoi.height() / r.height());
	}
	QSettings{}.setValue(Settings::PhotoCompareAoi, normalizedAoi);
}

QRectF PhotoCompareWindow::referenceSubjectRect() const
{
	// The reference's image rect placed into subject space. Assumes the reference carries no rotation - which
	// holds for the default alignment and, after an auto-align/calibration fold, by construction; the same
	// assumption is baked into the default alignment and the view fit that also build this rect.
	const Photo& ref = m_photos[m_refIndex];
	return QRectF(ref.alignOffset, QSizeF(ref.image.size()) * ref.alignScale);
}

void PhotoCompareWindow::setDefaultAlignment(Photo& photo)
{
	const QRectF refRect = referenceSubjectRect();
	photo.alignScale = refRect.height() / photo.image.height();
	photo.alignRotation = 0.0;
	photo.alignOffset = refRect.center() - photo.alignScale * QPointF(photo.image.width(), photo.image.height()) / 2.0;
}

void PhotoCompareWindow::resetToInitialState()
{
	if (m_photos.empty())
		return;
	if (m_calibrating)
		setCalibrating(false);
	exitFullView();
	setDifferenceMode(false);
	m_flickerIndex = -1;
	// The current reference keeps its role and defines subject space again (identity); every other photo
	// returns to the default height-normalized alignment against it - the same layout a fresh open produces,
	// only anchored on whichever photo is the reference now rather than forcing it back to photo 1.
	Photo& ref = m_photos[m_refIndex];
	ref.alignScale = 1.0;
	ref.alignRotation = 0.0;
	ref.alignOffset = QPointF();
	for (size_t i = 0; i < m_photos.size(); ++i)
	{
		Photo& photo = m_photos[i];
		photo.alignMarks.clear();
		photo.alignScored = false;
		if (static_cast<int>(i) != m_refIndex)
			setDefaultAlignment(photo);  // reads the now-identity reference
	}
	m_alignAoi = QRectF();
	m_viewTouched = false;  // like a fresh open: pane resizes re-fit again until the user navigates
	fitView();
	updateHintText();
	updateAllPanes();
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
		photo.filePath = path;
		photo.caption = QFileInfo(path).fileName();
		// Default alignment: normalize the photo's height to the reference's subject-space height and center
		// the two on each other - so identical shots that only differ in export resolution line up with no
		// user action at all. The very first photo keeps the identity default: it defines subject space.
		if (!m_photos.empty())
			setDefaultAlignment(photo);
		m_photos.push_back(std::move(photo));
	}

	rebuildPaneGrid();
	m_slider->setRange(0, std::max(0, static_cast<int>(m_photos.size()) - 1));
	m_slider->setEnabled(!m_photos.empty() && !m_calibrating);
	if (!m_viewTouched)
		fitView();
	// The first photos to arrive (the constructor batch, or the first drop into a window opened empty) get the
	// saved align region restored - only now does a reference exist to un-normalize the stored frame-fractions
	// against. Apply-once, and never over a region the user has already drawn.
	if (oldCount == 0 && !m_photos.empty() && m_alignAoi.isEmpty())
	{
		const QRectF normalizedAoi = QSettings{}.value(Settings::PhotoCompareAoi).toRectF();
		if (!normalizedAoi.isEmpty())
		{
			const QRectF r = referenceSubjectRect();
			m_alignAoi = QRectF(r.x() + normalizedAoi.x() * r.width(), r.y() + normalizedAoi.y() * r.height(),
			                    normalizedAoi.width() * r.width(), normalizedAoi.height() * r.height());
		}
	}
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
	else if (key == Qt::Key_R && !event->isAutoRepeat() && !m_photos.empty())
		resetToInitialState();
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
	const QRectF subjectRect = referenceSubjectRect();
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
	{
		photo.alignMarks.clear();
		photo.alignScored = false;  // re-derived below for each non-reference photo; the reference stays unscored
	}

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
	// The user's align region lives in subject space, which the fold just rebased to the reference's pixel
	// coords - rebase the rect along (which also makes it directly usable as the library's areaOfInterest).
	if (!m_alignAoi.isEmpty())
		m_alignAoi = subjectRectToImage(m_alignAoi, refScale, refRotation, refOffset);

	for (size_t i = 0; i < m_photos.size(); ++i)
	{
		if (static_cast<int>(i) == m_refIndex)
			continue;
		Photo& photo = m_photos[i];
		AlignmentOptions options;
		options.areaOfInterest = m_alignAoi;  // empty = whole frame
		options.fitRotation = !m_ignoreRotationCheck->isChecked();
		// refTransform^-1 * photoTransform: the photo's mapping into the rebased subject space.
		options.initialGuess = { photo.alignScale / refScale, photo.alignRotation - refRotation,
		                         rotated(photo.alignOffset - refOffset, -refRotation) / refScale };
		QElapsedTimer alignTimer;
		alignTimer.start();
		const AlignmentResult result = alignImages(ref.image, photo.image, options);
		photo.alignTimeMs = alignTimer.nsecsElapsed() / 1e6;
		m_alignMarkSize = result.patchSize;  // in reference px == subject units (the subject space was just rebased)
		photo.alignConfidence = result.confidence;        // all surfaced in this photo's corner caption, success or not
		photo.alignBootstrapZncc = result.bootstrapZncc;
		photo.alignRotationSigma = result.rotationSigma;
		photo.alignSucceeded = result.succeeded;
		photo.alignScored = true;
		//qDebug() << "Auto-align photo" << (i + 1) << "vs" << (m_refIndex + 1) << ": succeeded" << result.succeeded
		//         << " confidence" << result.confidence << " coarse" << result.bootstrapZncc
		//         << " scale" << result.transform.scale << " offset" << result.transform.offset
		//         << " rotation" << result.transform.rotation * (180.0 / Pi) << "+-" << result.rotationSigma * (180.0 / Pi) << "deg"
		//         << " radial k" << result.radialK << " time" << photo.alignTimeMs << "ms";
		static constexpr const char* fateNames[] = { "accepted", "accepted (coarse)", "outside overlap", "flat", "weak match", "outlier" };
		for (const AlignmentPatchInfo& patchInfo : result.patches)
		{
			//qDebug() << "  patch @" << patchInfo.refPoint << fateNames[static_cast<int>(patchInfo.fate)]
			//         << " zncc" << patchInfo.zncc << " residual" << patchInfo.residual;
			const auto kind = patchInfo.fate == AlignmentPatchFate::Accepted ? AlignmentMark::Kind::Used
			                : patchInfo.fate == AlignmentPatchFate::AcceptedCoarse ? AlignmentMark::Kind::UsedCoarse
			                : patchInfo.fate == AlignmentPatchFate::Outlier ? AlignmentMark::Kind::Outlier
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
		}
		else  // keep the (rebased) current alignment; the caption's conf/coarse scores tell the story
		{
			photo.alignScale = options.initialGuess.scale;
			photo.alignRotation = options.initialGuess.rotation;
			photo.alignOffset = options.initialGuess.offset;
		}
	}
	QApplication::restoreOverrideCursor();
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
		photo.alignScored = false;  // and the auto-align scores they went with
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
	if (!m_alignAoi.isEmpty())  // subject space is being rebased - keep the align region glued to its content
		m_alignAoi = subjectRectToImage(m_alignAoi, ref.alignScale, ref.alignRotation, ref.alignOffset);
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
		m_hintLabel->setText(tr("Full view %1/%2 · slider / Left,Right: switch photo · hold 1..%2: flicker · A: auto-align · Shift+drag: align region · D: difference · wheel: zoom · drag: pan · Ctrl+wheel / Ctrl+drag: adjust this photo · F: fit · R: reset · Esc: back to grid")
			.arg(m_fullViewIndex + 1).arg(m_photos.size()));
	else
		m_hintLabel->setText(tr("Wheel: zoom · drag: pan · Ctrl+wheel / Ctrl+drag: adjust one photo · A: auto-align · Shift+A: align by 2 clicks · Shift+drag: align region · hold 1..%1: flicker · D: difference · slider: full view · F / double-click: fit · R: reset · Esc: close")
			.arg(m_photos.size()));
}
