#include "Windows/PhotoCompareWindow.h"
#include "Core/IoThreadPool.h"
#include "MagicAlignment.h"
#include "Theme/Theme.h"
#include "UiComponents/SegmentedToggle.h"
#include "Utils.h"

#include "assert/advanced_assert.h"
#include "utils/naturalsorting/cnaturalsorterqcollator.h"

#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QContextMenuEvent>
#include <QDebug>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFile>
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
#include <QPolygonF>
#include <QSettings>
#include <QSlider>
#include <QStackedLayout>
#include <QtMath>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <numbers>
#include <thread>

// This window's persisted UI state. These keys live here, not in Settings.h, because they are local to the
// compare tool rather than app-wide configuration - but stay under the Settings namespace for the uniform
// QSettings{}.value(Settings::Foo) call style. The align region is stored as a QRectF of fractions of the
// reference frame (resolution-independent); an empty/absent value means no region.
namespace Settings {
	constexpr const char* PhotoCompareIgnoreRotation = "photoCompare/ignoreRotation";
	constexpr const char* PhotoCompareAoi = "photoCompare/aoiNormalized";
}

namespace {

// The compare window shows at most this many photos: each holds a full-resolution image plus a mipmap chain, so
// the working set stays bounded, and beyond roughly this count an N-way visual comparison stops being useful.
constexpr qsizetype MaxImages = 50;

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
	QPolygonF corners;
	for (const QPointF& corner : { rect.topLeft(), rect.topRight(), rect.bottomLeft(), rect.bottomRight() })
		corners.push_back(rotated(corner - offset, -rotation) / scale);
	return corners.boundingRect();
}

} // namespace

// The load work and results of one addPhotosFromFiles call, shared between the GUI thread and the two loading
// stages (the file reads on the process-wide I/O thread, the decodes on _workerPool).
struct PhotoCompareWindow::PhotoLoadBatch
{
	QStringList paths;
	std::vector<QImage> images;  // [i] decoded from paths[i]; null = the file failed to load
	QString completionNotice;    // transient status to show once the batch is applied (e.g. the drop-truncation notice)
	std::atomic<int> completedCount{ 0 };  // decoded-or-failed so far; drives the progress line in updateHintText
	std::atomic<bool> abort{ false };      // set by the destructor: the remaining reads and decodes become no-ops, so its waits stay short
};

// One grid cell: a viewport onto the shared view. All state (images, alignment, the view itself) lives in
// the owning PhotoCompareWindow; the pane renders and translates mouse input into owner calls.
// The same class also serves as the full-view pane (index -1), which shows whatever photo the owner's
// _fullViewIndex selects - photoIndex() resolves that indirection.
class PhotoComparePane final : public QWidget
{
public:
	PhotoComparePane(PhotoCompareWindow& owner, int index) : QWidget(&owner), _owner(owner), _index(index)
	{
		setMinimumSize(50, 50);
		setCursor(_owner.idleCursor());
	}

protected:
	void paintEvent(QPaintEvent*) override;
	void wheelEvent(QWheelEvent* event) override;
	void mousePressEvent(QMouseEvent* event) override;
	void mouseMoveEvent(QMouseEvent* event) override;
	void mouseReleaseEvent(QMouseEvent* event) override;
	void mouseDoubleClickEvent(QMouseEvent* event) override;
	void contextMenuEvent(QContextMenuEvent* event) override;
	void resizeEvent(QResizeEvent* event) override { QWidget::resizeEvent(event); _owner.onPaneResized(); }

private:
	// The photo this pane represents: its own grid position, or the slider-picked one for the full-view pane.
	[[nodiscard]] int photoIndex() const { return _index >= 0 ? _index : _owner._fullViewIndex; }

	void drawCaption(QPainter& painter, const PhotoCompareWindow::Photo& photo, int renderIndex) const;

	PhotoCompareWindow& _owner;
	const int _index;  // grid/photo index, or -1 for the full-view pane

	// Click-vs-drag: a press starts drag tracking; a release that never crossed the threshold is a click
	// (which is how calibration points are placed - so panning stays available while calibrating).
	QPointF _pressPos;
	QPointF _lastDragPos;
	bool _leftButtonDown = false;
	bool _dragConfirmed = false;
	bool _ctrlDrag = false;

	// Shift+drag rubber-bands the owner's auto-align region; the anchor is held in subject space so the
	// rect stays glued to the content even if the view is somehow moved mid-drag.
	bool _aoiDrag = false;
	QPointF _aoiAnchor;
};

void PhotoComparePane::paintEvent(QPaintEvent*)
{
	QPainter painter(this);
	painter.fillRect(rect(), QColor(Theme::current().ThumbnailMatte));

	const int renderIndex = _owner._flickerIndex >= 0 ? _owner._flickerIndex : photoIndex();
	PhotoCompareWindow::Photo& photo = _owner._photos[renderIndex];

	const auto drawPhoto = [&](PhotoCompareWindow::Photo& drawn) {
		const double drawnScale = _owner._viewZoom * drawn.alignScale;
		double residualScale = 1.0;
		const QImage& source = _owner.imageForScale(drawn, drawnScale, devicePixelRatioF(), residualScale);
		painter.save();
		painter.setRenderHint(QPainter::SmoothPixmapTransform);
		painter.translate(_owner._viewZoom * drawn.alignOffset + _owner._viewPan);
		painter.rotate(qRadiansToDegrees(drawn.alignRotation));  // rotation and uniform scale commute, so the order is free
		painter.scale(residualScale, residualScale);
		painter.drawImage(0, 0, source);
		painter.restore();
	};

	// Difference mode renders every photo except the reference as its per-channel |photo - reference|:
	// the reference is drawn first, the photo on top of it in Difference composition mode. Where only one
	// of the two covers, that image differences against the matte, i.e. shows (nearly) unchanged.
	if (_owner._differenceMode && renderIndex != _owner._refIndex)
	{
		drawPhoto(_owner._photos[_owner._refIndex]);
		painter.save();
		painter.setCompositionMode(QPainter::CompositionMode_Difference);
		drawPhoto(photo);
		painter.restore();
	}
	else
		drawPhoto(photo);

	painter.setRenderHint(QPainter::Antialiasing);

	// Calibration crosshairs - the pane's OWN points ('photo' is exactly that: flicker is inert while calibrating).
	if (_owner._calibrating)
	{
		painter.setPen(QPen(QColor(Theme::current().AccentBorder), 2));
		for (const QPointF& imagePos : photo.calibPoints)
		{
			const QPointF c = _owner.widgetFromImage(photo, imagePos);
			painter.drawLine(c - QPointF(8, 0), c + QPointF(8, 0));
			painter.drawLine(c - QPointF(0, 8), c + QPointF(0, 8));
		}
	}

	// Auto-align diagnostics: where the aligner took its evidence in the RENDERED photo, drawn at the
	// patch's true on-screen footprint. Accent = used for the fit (dashed = used via a coarser-level match
	// only, e.g. defocused at full res); orange = matched well but inconsistent with the fitted transform
	// (outlier - locally moved content, parallax, ...); red = failed to match.
	if (_owner._showAlignDiagnostics)
	{
		painter.setBrush(Qt::NoBrush);
		const double markHalf = 0.5 * _owner._alignMarkSize * _owner._viewZoom;
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
			const QPointF center = _owner.widgetFromImage(photo, mark.imagePos);
			painter.drawRect(QRectF(center.x() - markHalf, center.y() - markHalf, 2.0 * markHalf, 2.0 * markHalf));
		}
	}

	// The auto-align region (Shift+drag): one subject-space rect, so it frames the same content in every
	// pane. Dashed light gray, to stay distinct from the accent patch marks.
	if (!_owner._alignAoi.isEmpty())
	{
		painter.setPen(QPen(QColor(0xe8, 0xe8, 0xe8), 1, Qt::DashLine));
		painter.setBrush(Qt::NoBrush);
		painter.drawRect(QRectF(_owner._viewZoom * _owner._alignAoi.topLeft() + _owner._viewPan,
		                        _owner._alignAoi.size() * _owner._viewZoom));
	}

	drawCaption(painter, photo, renderIndex);

	// The reference pane - the photo difference mode and both alignment paths work against - is outlined in yellow
	if (photoIndex() == _owner._refIndex && _owner._photos.size() > 1)
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

// Corner caption, stacked lines. Headline "2 · name.jpg · 6000x4000 (24 MP) · 63%": the leading digit
// doubles as the pane's flicker key, then the photo's pixel resolution, then its on-screen scale in device
// pixels (100% = 1 image px per physical device px, i.e. the Ctrl+1 actual-pixels view), making any
// compensation difference between panes visible at a glance. The
// alignment line spells out this photo's raw similarity into subject space (scale, rotation in degrees,
// offset in subject px). The score line appears only once auto-align has evaluated this photo: the run's
// two quality measures (conf = weighted patch ZNCC fitness, coarse = coarse whole-frame score), the fitted
// rotation's 1-sigma error bar, and the align call's runtime.
void PhotoComparePane::drawCaption(QPainter& painter, const PhotoCompareWindow::Photo& photo, int renderIndex) const
{
	QStringList captionLines;
	captionLines << QString("%1 · %2 · %3x%4 (%5 MP) · %6%")
		.arg(renderIndex + 1).arg(photo.caption)
		.arg(photo.image.width()).arg(photo.image.height())
		.arg(qRound(photo.image.width() * photo.image.height() / 1e6))
		.arg(qRound(_owner._viewZoom * photo.alignScale * devicePixelRatioF() * 100.0));
	captionLines << QString("scale %1 · rot %2° · offset (%3, %4)")
		.arg(photo.alignScale, 0, 'f', 3)
		.arg(qRadiansToDegrees(photo.alignRotation), 0, 'f', 2)
		.arg(qRound(photo.alignOffset.x())).arg(qRound(photo.alignOffset.y()));
	if (photo.alignScored)
	{
		QString scoreLine = QString("conf %1 · coarse %2 · rot ±%3° · %4 ms")
			.arg(photo.alignConfidence, 0, 'f', 2).arg(photo.alignBootstrapZncc, 0, 'f', 2)
			.arg(qRadiansToDegrees(photo.alignRotationSigma), 0, 'f', 2).arg(photo.alignTimeMs, 0, 'f', 0);
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
}

void PhotoComparePane::wheelEvent(QWheelEvent* event)
{
	const double steps = event->angleDelta().y() / 120.0;
	if (steps == 0.0)
		return;
	const double factor = std::pow(1.25, steps);
	if (event->modifiers().testFlag(Qt::ControlModifier))
		_owner.adjustPhotoScale(photoIndex(), factor, event->position());
	else
		_owner.zoomView(factor, event->position());
	event->accept();
}

void PhotoComparePane::mousePressEvent(QMouseEvent* event)
{
	if (event->button() == Qt::LeftButton && event->modifiers().testFlag(Qt::ShiftModifier))
	{
		// Shift+drag marks the auto-align region; clearing on press makes a plain Shift+click the eraser.
		_aoiDrag = true;
		_aoiAnchor = _owner.subjectFromWidget(event->position());
		_owner._alignAoi = QRectF();
		_owner.updateAllPanes();
		setCursor(Qt::CrossCursor);
	}
	else if (event->button() == Qt::LeftButton)
	{
		_leftButtonDown = true;
		_dragConfirmed = false;
		_ctrlDrag = event->modifiers().testFlag(Qt::ControlModifier);
		_pressPos = _lastDragPos = event->position();
	}
	else if (event->button() == Qt::RightButton && _owner._calibrating)
		_owner.undoCalibrationPoint(photoIndex());
}

void PhotoComparePane::mouseMoveEvent(QMouseEvent* event)
{
	if (_aoiDrag)
	{
		_owner._alignAoi = QRectF(_aoiAnchor, _owner.subjectFromWidget(event->position())).normalized();
		_owner.updateAllPanes();
		return;
	}
	if (!_leftButtonDown)
		return;
	const QPointF pos = event->position();
	if (!_dragConfirmed && (pos - _pressPos).manhattanLength() > 4.0)
	{
		_dragConfirmed = true;
		setCursor(Qt::ClosedHandCursor);
	}
	if (_dragConfirmed)
	{
		if (_ctrlDrag)
			_owner.movePhotoOffset(photoIndex(), pos - _lastDragPos);
		else
			_owner.panView(pos - _lastDragPos);
	}
	_lastDragPos = pos;
}

void PhotoComparePane::mouseReleaseEvent(QMouseEvent* event)
{
	if (event->button() == Qt::LeftButton && _aoiDrag)
	{
		_aoiDrag = false;
		setCursor(_owner.idleCursor());
		// A degenerate rect (a stray wiggle of a clearing Shift+click) is not a usable region - drop it.
		if (std::min(_owner._alignAoi.width(), _owner._alignAoi.height()) * _owner._viewZoom < 8.0)
			_owner._alignAoi = QRectF();
		_owner.updateAllPanes();
		return;
	}
	if (event->button() != Qt::LeftButton || !_leftButtonDown)
		return;
	_leftButtonDown = false;
	setCursor(_owner.idleCursor());
	if (!_dragConfirmed && _owner._calibrating)
		_owner.addCalibrationPoint(photoIndex(), _owner.imageFromWidget(_owner._photos[photoIndex()], _pressPos));
}

void PhotoComparePane::mouseDoubleClickEvent(QMouseEvent* event)
{
	// While calibrating, a double-click is just a second press - route it as one so its release goes through
	// the normal click path (addCalibrationPoint's proximity guard swallows the would-be duplicate point).
	if (_owner._calibrating)
		mousePressEvent(event);
	else if (event->button() == Qt::LeftButton)
		_owner.fitView();
}

void PhotoComparePane::contextMenuEvent(QContextMenuEvent* event)
{
	if (_owner._calibrating)  // right-click means "undo a point" while calibrating
		return;
	const int index = photoIndex();
	QMenu menu;
	menu.addAction(tr("Open containing folder"), [this, index] {
		if (const QString path = _owner._photos[index].filePath; !revealInFileManager(path))
			reportMissingFile(this, path);
	});
	QAction* makeReference = menu.addAction(tr("Make this the reference image"), [this, index] {
		_owner._refIndex = index;
		_owner.updateAllPanes();  // difference mode and the next align/calibration now work against this photo
	});
	makeReference->setEnabled(index != _owner._refIndex);
	menu.exec(event->globalPos());
}

// ---------------------------------------------------------------------------------------------------------

void PhotoCompareWindow::showForFiles(const QStringList& candidatePaths, QWidget* parent)
{
	QStringList paths;
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

PhotoCompareWindow::PhotoCompareWindow(const QStringList& photoPaths, QWidget* parent) : QWidget(parent, Qt::Window),
	// Hardware threads minus one, so the GUI thread keeps a core while the workers decode and align
	_workerPool(std::max(std::thread::hardware_concurrency(), 2u) - 1, "photo-compare")
{
	setWindowTitle(tr("Compare Photos"));
	setFocusPolicy(Qt::StrongFocus);  // the panes never take focus, so key events land here
	setAcceptDrops(true);  // no child accepts drops, so a drop anywhere in the window lands here

	QVBoxLayout* mainLayout = new QVBoxLayout(this);
	mainLayout->setContentsMargins(4, 4, 4, 4);

	_gridPage = new QWidget(this);
	_dropHintLabel = new QLabel(_gridPage);  // the text is set in rebuildPaneGrid (drop prompt, or a loading notice)
	_dropHintLabel->setAlignment(Qt::AlignCenter);

	_fullPane = new PhotoComparePane(*this, -1);
	_viewStack = new QStackedLayout();
	_viewStack->addWidget(_gridPage);
	_viewStack->addWidget(_fullPane);
	mainLayout->addLayout(_viewStack, 1);

	// Bottom toolbar: the full-view picker slider stretching across; the align-option checkbox and the
	// render-mode toggle at the right.
	QHBoxLayout* toolbar = new QHBoxLayout();

	// Full-view picker: one detent per photo; pressing the handle or changing the value enters the full
	// view, dragging back and forth scrubs between the aligned photos (a flicker gesture at full size).
	_slider = new QSlider(Qt::Horizontal, this);
	_slider->setPageStep(1);
	_slider->setTickPosition(QSlider::TicksBelow);
	_slider->setTickInterval(1);
	_slider->setFocusPolicy(Qt::NoFocus);  // all keyboard input stays on the window
	connect(_slider, &QSlider::sliderPressed, this, [this] { setFullViewIndex(_slider->value()); });
	connect(_slider, &QSlider::valueChanged, this, [this](int value) { setFullViewIndex(value); });
	toolbar->addWidget(_slider, 1);

	_ignoreRotationCheck = new QCheckBox(tr("Ignore rotation"), this);
	_ignoreRotationCheck->setToolTip(tr("Auto-align fits scale and offset only, treating any apparent rotation as spurious\n"
	                                     "(e.g. depth parallax between focus-stack slices can read as a slight tilt)"));
	_ignoreRotationCheck->setFocusPolicy(Qt::NoFocus);  // all keyboard input stays on the window
	_ignoreRotationCheck->setChecked(QSettings{}.value(Settings::PhotoCompareIgnoreRotation, false).toBool());
	connect(_ignoreRotationCheck, &QCheckBox::toggled, this,
	        [](bool checked) { QSettings{}.setValue(Settings::PhotoCompareIgnoreRotation, checked); });
	toolbar->addWidget(_ignoreRotationCheck, 0);

	_diffToggle = new SegmentedToggle({ tr("Normal"), tr("Difference") }, this);
	_diffToggle->setToolTip(tr("Difference: render each photo as its per-pixel difference against the reference photo (D)"));
	connect(_diffToggle, &SegmentedToggle::currentChanged, this, [this](int index) { setDifferenceMode(index == 1); });
	toolbar->addWidget(_diffToggle, 0);

	mainLayout->addLayout(toolbar, 0);

	_hintLabel = new QLabel(this);
	_hintLabel->setAlignment(Qt::AlignCenter);
	mainLayout->addWidget(_hintLabel, 0);

	addPhotosFromFiles(photoPaths);  // async: the photos appear once decoded; an empty list puts the empty drop-target
	                                 // state in place immediately (the saved align region is restored on apply, once
	                                 // the first photos actually exist)

	if (!restoreWindowGeometry(this, "photoCompareWindow"))
	{
		resize(1200, 800);  // the size the window falls back to when un-maximized
		setWindowState(Qt::WindowMaximized);  // "use the whole screen" by default; the saved geometry rules thereafter
	}
}

PhotoCompareWindow::~PhotoCompareWindow()
{
	if (_loadBatch)
	{
		_loadBatch->abort = true;  // the remaining reads and decodes become no-ops, bounding the waits below to one file each
		// After retire() the read loop is gone (not merely aborted): it can no longer touch the dying window or
		// enqueue into _workerPool. The decodes already enqueued are dropped or joined by _workerPool's destruction.
		IoThreadPool::retire(reinterpret_cast<uint64_t>(this));
	}
	saveWindowGeometry(this, "photoCompareWindow");
	// Persist the align region as fractions of the reference frame (resolution-independent). Empty (no region,
	// or no photos to anchor it) stores an empty rect, which reads back as "no region".
	const QRectF normalizedAoi = !_alignAoi.isEmpty() && !_photos.empty() ? normalizedFromSubjectRect(_alignAoi) : QRectF();
	QSettings{}.setValue(Settings::PhotoCompareAoi, normalizedAoi);
}

QRectF PhotoCompareWindow::normalizedFromSubjectRect(const QRectF& rect) const
{
	const QRectF r = referenceSubjectRect();
	return QRectF((rect.x() - r.x()) / r.width(), (rect.y() - r.y()) / r.height(),
	              rect.width() / r.width(), rect.height() / r.height());
}

QRectF PhotoCompareWindow::subjectRectFromNormalized(const QRectF& normalized) const
{
	const QRectF r = referenceSubjectRect();
	return QRectF(r.x() + normalized.x() * r.width(), r.y() + normalized.y() * r.height(),
	              normalized.width() * r.width(), normalized.height() * r.height());
}

QRectF PhotoCompareWindow::referenceSubjectRect() const
{
	// The reference's image rect placed into subject space. Assumes the reference carries no rotation - which
	// holds for the default alignment and, after an auto-align/calibration fold, by construction; the same
	// assumption is baked into the default alignment and the view fit that also build this rect.
	const Photo& ref = _photos[_refIndex];
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
	if (_photos.empty())
		return;
	if (_calibrating)
		setCalibrating(false);
	exitFullView();
	setDifferenceMode(false);
	_flickerIndex = -1;
	// The current reference keeps its role and defines subject space again (identity); every other photo
	// returns to the default height-normalized alignment against it - the same layout a fresh open produces,
	// only anchored on whichever photo is the reference now rather than forcing it back to photo 1.
	Photo& ref = _photos[_refIndex];
	ref.alignScale = 1.0;
	ref.alignRotation = 0.0;
	ref.alignOffset = QPointF();
	for (size_t i = 0; i < _photos.size(); ++i)
	{
		Photo& photo = _photos[i];
		photo.alignMarks.clear();
		photo.alignScored = false;
		if (static_cast<int>(i) != _refIndex)
			setDefaultAlignment(photo);  // reads the now-identity reference
	}
	_alignAoi = QRectF();
	_viewTouched = false;  // like a fresh open: pane resizes re-fit again until the user navigates
	fitView();
	updateHintText();
	updateAllPanes();
}

void PhotoCompareWindow::addPhotosFromFiles(const QStringList& photoPaths)
{
	assert_debug_only(!_loadBatch);  // drops are denied while a batch is in flight, so batches never overlap
	if (photoPaths.isEmpty())
	{
		PhotoLoadBatch emptyBatch;
		applyLoadedPhotoBatch(emptyBatch);  // still (re)builds the count-dependent state - the empty drop-target UI
		return;
	}

	auto batch = std::make_shared<PhotoLoadBatch>();
	batch->paths = photoPaths;
	batch->images.resize(static_cast<size_t>(photoPaths.size()));
	_loadBatch = batch;
	if (_photos.empty())
	{
		rebuildPaneGrid();  // no photos to show while decoding: put the centered "Loading photos..." placeholder in the grid page
		_slider->setEnabled(false);
	}
	updateHintText();

	// Two-stage load (see ARCHITECTURE.md "Core principles"). Stage 1, the process-wide I/O pool (routed by the
	// storage medium under the batch's first path): read the files one at a time, handing each one's bytes to
	// the compute pool to decode as soon as they arrive. Tagged with this window so the destructor can retire()
	// the read loop. The tasks own a share of the batch state, so nothing here outlives the window's teardown guarantees.
	IoThreadPool::enqueue(batch->paths.front(), [this, batch] {
		for (qsizetype i = 0; i < batch->paths.size(); ++i)
		{
			if (batch->abort)
				return;
			QFile file(batch->paths[i]);
			QByteArray fileBytes;
			if (file.open(QIODevice::ReadOnly))
				fileBytes = file.readAll();
			else
				qWarning() << "PhotoCompareWindow: failed to read" << batch->paths[i] << "-" << file.errorString();
			// Stage 2, the compute pool: decode in parallel; the decode that completes the batch hands the whole
			// ordered batch to the GUI thread.
			_workerPool.enqueue([this, batch, i, fileBytes = std::move(fileBytes)]() mutable {
				if (!batch->abort)
				{
					QBuffer buffer(&fileBytes);
					QImageReader reader(&buffer);  // Do not provide a format hint, that suppresses detection by contents
					reader.setAutoTransform(true);  // apply the EXIF orientation
					QImage& image = batch->images[static_cast<size_t>(i)];
					image = reader.read();
					if (image.isNull())
						qWarning() << "PhotoCompareWindow: failed to decode" << batch->paths[i] << "-" << reader.errorString();
				}
				if (++batch->completedCount == static_cast<int>(batch->paths.size()))
					QMetaObject::invokeMethod(this, [this, batch] { applyLoadedPhotoBatch(*batch); }, Qt::QueuedConnection);
				else
					// The _loadBatch check keeps a late progress update from overwriting a transient completion notice
					QMetaObject::invokeMethod(this, [this] { if (_loadBatch) updateHintText(); }, Qt::QueuedConnection);
			});
		}
	}, reinterpret_cast<uint64_t>(this));
}

void PhotoCompareWindow::applyLoadedPhotoBatch(PhotoLoadBatch& batch)
{
	_loadBatch.reset();

	const size_t oldCount = _photos.size();
	for (qsizetype i = 0; i < batch.paths.size(); ++i)
	{
		QImage& image = batch.images[static_cast<size_t>(i)];
		if (image.isNull())
			continue;  // failed to load; the decode already logged the warning
		Photo photo;
		photo.image = std::move(image);
		photo.filePath = batch.paths[i];
		photo.caption = QFileInfo(batch.paths[i]).fileName();
		// Default alignment: normalize the photo's height to the reference's subject-space height and center
		// the two on each other - so identical shots that only differ in export resolution line up with no
		// user action at all. The very first photo keeps the identity default: it defines subject space.
		if (!_photos.empty())
			setDefaultAlignment(photo);
		_photos.push_back(std::move(photo));
	}

	rebuildPaneGrid();
	_slider->setRange(0, std::max(0, static_cast<int>(_photos.size()) - 1));
	_slider->setEnabled(!_photos.empty() && !_calibrating);
	if (!_viewTouched)
		fitView();
	// The first photos to arrive (the constructor batch, or the first drop into a window opened empty) get the
	// saved align region restored - only now does a reference exist to un-normalize the stored frame-fractions
	// against. Apply-once, and never over a region the user has already drawn.
	if (oldCount == 0 && !_photos.empty() && _alignAoi.isEmpty())
	{
		const QRectF normalizedAoi = QSettings{}.value(Settings::PhotoCompareAoi).toRectF();
		if (!normalizedAoi.isEmpty())
			_alignAoi = subjectRectFromNormalized(normalizedAoi);
	}
	updateHintText();
	updateAllPanes();
	// Transient status, like the auto-align summary: set after updateHintText, which would otherwise overwrite it
	if (_photos.size() == oldCount && !batch.paths.isEmpty())
		_hintLabel->setText(tr("None of the files could be loaded as images."));
	else if (!batch.completionNotice.isEmpty())
		_hintLabel->setText(batch.completionNotice);
}

void PhotoCompareWindow::rebuildPaneGrid()
{
	// Rebuilt from scratch on every photo count change: recreate the layout (panes stay children of the page)
	// and re-place every pane - a fresh layout also drops the previous geometry's row/column stretches.
	delete _gridPage->layout();
	QGridLayout* grid = new QGridLayout(_gridPage);
	grid->setContentsMargins(0, 0, 0, 0);
	grid->setSpacing(4);

	const int photoCount = static_cast<int>(_photos.size());
	_dropHintLabel->setVisible(photoCount == 0);
	if (photoCount == 0)
	{
		_dropHintLabel->setText(_loadBatch ? tr("Loading photos...") : tr("Drop images or folders here to compare them"));
		grid->addWidget(_dropHintLabel, 0, 0);
		return;
	}

	while (static_cast<int>(_paneWidgets.size()) < photoCount)
		_paneWidgets.push_back(new PhotoComparePane(*this, static_cast<int>(_paneWidgets.size())));

	const int columns = static_cast<int>(std::ceil(std::sqrt(photoCount)));
	const int rows = (photoCount + columns - 1) / columns;
	for (int i = 0; i < photoCount; ++i)
		grid->addWidget(_paneWidgets[i], i / columns, i % columns);
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
	if (_loadBatch)
		return;  // one batch at a time: deny drops until the current load is applied
	// Accept any local path; folders are expanded and non-images filtered out on drop.
	const QList<QUrl> urls = event->mimeData()->urls();
	if (std::any_of(urls.cbegin(), urls.cend(), [](const QUrl& url) { return url.isLocalFile(); }))
		event->acceptProposedAction();
}

void PhotoCompareWindow::dropEvent(QDropEvent* event)
{
	// A dropped folder is scanned recursively for image files (so a whole shoot drops in at once); plain files
	// pass straight to the load attempt in addPhotosFromFiles. The total is capped at MaxImages, counting photos
	// already loaded: each photo holds a full-resolution image plus its mipmap chain, so an unbounded folder would
	// exhaust memory - and, as in showForFiles, a larger set stops being a useful comparison.
	QStringList paths;
	for (const QUrl& url : event->mimeData()->urls())
	{
		if (!url.isLocalFile())
			continue;
		const QString localPath = url.toLocalFile();
		if (!QFileInfo(localPath).isDir())
		{
			paths.push_back(localPath);
			continue;
		}
		QStringList folderImages = collectFilesInDirectory(localPath, /*recursive=*/true, isSupportedImageFile);
		// filesystem order is unspecified; sort so the panes follow file-name order
		std::ranges::sort(folderImages, &NaturalSort::lessCaseSensitive);
		paths += folderImages;
	}

	const qsizetype capacity = std::max<qsizetype>(0, MaxImages - static_cast<qsizetype>(_photos.size()));
	const bool truncated = paths.size() > capacity;
	if (truncated)
		paths.resize(capacity);
	addPhotosFromFiles(paths);
	if (truncated)
	{
		const QString notice = tr("The comparison is limited to %1 photos; the remaining dropped files were skipped.").arg(MaxImages);
		if (_loadBatch)
			_loadBatch->completionNotice = notice;  // shown on apply; the load's own hint updates would overwrite it if set now
		else
			_hintLabel->setText(notice);  // capacity was 0, so no load started and nothing will overwrite the notice
	}
	event->acceptProposedAction();
}

void PhotoCompareWindow::keyPressEvent(QKeyEvent* event)
{
	const int key = event->key();
	const bool ctrl = event->modifiers().testFlag(Qt::ControlModifier);
	if (key == Qt::Key_Escape)
	{
		if (_fullViewIndex >= 0)
			exitFullView();
		else if (_calibrating)
			setCalibrating(false);
		else
			close();
	}
	else if (key == Qt::Key_A && !event->isAutoRepeat() && !_photos.empty())
	{
		if (event->modifiers().testFlag(Qt::ShiftModifier))
		{
			exitFullView();  // calibration points are placed per grid pane
			setCalibrating(!_calibrating);
		}
		else
			autoAlignPhotos();  // works in the full view too
	}
	else if (key == Qt::Key_F || key == Qt::Key_Home || (ctrl && key == Qt::Key_0))
		fitView();
	else if (ctrl && key == Qt::Key_1)
		zoomToActualPixels();
	else if (key == Qt::Key_D && !event->isAutoRepeat() && !_photos.empty())
		setDifferenceMode(!_differenceMode);
	else if (key == Qt::Key_I && !event->isAutoRepeat() && !_photos.empty())
	{
		_showAlignDiagnostics = !_showAlignDiagnostics;
		updateAllPanes();
	}
	else if (key == Qt::Key_R && !event->isAutoRepeat() && !_photos.empty())
		resetToInitialState();
	else if (_fullViewIndex >= 0 && (key == Qt::Key_Left || key == Qt::Key_Right))
		_slider->setValue(_slider->value() + (key == Qt::Key_Right ? 1 : -1));  // setValue clamps to the range
	else if (!ctrl && !_calibrating && !event->isAutoRepeat() &&
	         key >= Qt::Key_1 && key <= Qt::Key_9 && key < Qt::Key_1 + static_cast<int>(_photos.size()))
	{
		_flickerIndex = key - Qt::Key_1;
		updateAllPanes();
	}
	else
		QWidget::keyPressEvent(event);
}

void PhotoCompareWindow::keyReleaseEvent(QKeyEvent* event)
{
	if (!event->isAutoRepeat() && _flickerIndex >= 0 && event->key() == Qt::Key_1 + _flickerIndex)
	{
		_flickerIndex = -1;
		updateAllPanes();
	}
	else
		QWidget::keyReleaseEvent(event);
}

QPointF PhotoCompareWindow::subjectFromWidget(const QPointF& widgetPos) const
{
	return (widgetPos - _viewPan) / _viewZoom;
}

QPointF PhotoCompareWindow::widgetFromImage(const Photo& photo, const QPointF& imagePos) const
{
	return _viewZoom * (photo.alignScale * rotated(imagePos, photo.alignRotation) + photo.alignOffset) + _viewPan;
}

QPointF PhotoCompareWindow::imageFromWidget(const Photo& photo, const QPointF& widgetPos) const
{
	return rotated((subjectFromWidget(widgetPos) - photo.alignOffset) / photo.alignScale, -photo.alignRotation);
}

const QImage& PhotoCompareWindow::imageForScale(Photo& photo, double effectiveScale, double devicePixelRatio, double& residualScale)
{
	// Pick the halving-chain level against the PHYSICAL target (effectiveScale * devicePixelRatio): the widget
	// paints in logical units, but Qt blits to a devicePixelRatio-denser backing store, so choosing by the
	// logical scale alone hands a too-coarse mip to be upscaled on HiDPI. Selecting by the physical scale keeps
	// the painter's live bilinear pass minifying by <= 2x in REAL pixels - bilinear aliases badly past that (the
	// same lesson as ThumbnailWidget's pre-resample fix). Levels are built once, on demand.
	const double pickScale = effectiveScale * devicePixelRatio;
	int level = 0;
	double levelScale = 1.0;
	while (levelScale * 0.5 >= pickScale && level < 16)
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
	residualScale = effectiveScale * std::pow(2.0, level);  // logical: the painter still works in device-independent units
	return level == 0 ? photo.image : photo.mipmaps[level - 1];
}

void PhotoCompareWindow::zoomView(double factor, const QPointF& widgetAnchor)
{
	const double newZoom = std::clamp(_viewZoom * factor, 0.01, 100.0);
	const double applied = newZoom / _viewZoom;
	_viewZoom = newZoom;
	_viewPan = widgetAnchor - applied * (widgetAnchor - _viewPan);  // keeps the subject point under the cursor fixed
	_viewTouched = true;
	updateAllPanes();
}

void PhotoCompareWindow::panView(const QPointF& widgetDelta)
{
	_viewPan += widgetDelta;
	_viewTouched = true;
	updateAllPanes();
}

void PhotoCompareWindow::fitView()
{
	if (_photos.empty())
		return;
	const QSizeF paneSize = (_fullViewIndex >= 0 ? _fullPane : _paneWidgets[0])->size();
	if (paneSize.isEmpty())
		return;
	const QRectF subjectRect = referenceSubjectRect();
	_viewZoom = std::min(paneSize.width() / subjectRect.width(), paneSize.height() / subjectRect.height());
	_viewPan = QPointF((paneSize.width() - _viewZoom * subjectRect.width()) / 2.0,
	                    (paneSize.height() - _viewZoom * subjectRect.height()) / 2.0)
	            - _viewZoom * subjectRect.topLeft();
	updateAllPanes();
}

void PhotoCompareWindow::zoomToActualPixels()
{
	if (_photos.empty())
		return;
	const QWidget* pane = _fullViewIndex >= 0 ? _fullPane : _paneWidgets.front();
	const QSizeF paneSize = pane->size();
	if (paneSize.isEmpty())
		return;
	// The reference's on-screen scale is _viewZoom * alignScale in LOGICAL pixels; dividing by the pane's
	// devicePixelRatio is what makes it a true 1:1 with physical pixels on HiDPI rather than DPR-magnified.
	const double targetZoom = 1.0 / (pane->devicePixelRatioF() * _photos[_refIndex].alignScale);
	zoomView(targetZoom / _viewZoom, QPointF(paneSize.width() / 2.0, paneSize.height() / 2.0));  // keep the center fixed
}

void PhotoCompareWindow::adjustPhotoScale(int index, double factor, const QPointF& widgetAnchor)
{
	// Rescale one photo's alignment keeping ITS point under the cursor fixed (the other panes don't move).
	Photo& photo = _photos[index];
	const QPointF subjectAnchor = subjectFromWidget(widgetAnchor);
	const QPointF imageAnchor = imageFromWidget(photo, widgetAnchor);
	photo.alignScale = std::clamp(photo.alignScale * factor, 0.01, 100.0);
	photo.alignOffset = subjectAnchor - photo.alignScale * rotated(imageAnchor, photo.alignRotation);
	_viewTouched = true;
	updateAllPanes();  // flicker can be rendering this photo in other panes, so update all (it's <= 4 repaints)
}

void PhotoCompareWindow::movePhotoOffset(int index, const QPointF& widgetDelta)
{
	_photos[index].alignOffset += widgetDelta / _viewZoom;
	_viewTouched = true;
	updateAllPanes();
}

// Folds the reference's alignment into the view transform, making subject space the reference's pixel coords
// (the frame both alignment paths work in) while the reference stays pixel-frozen on screen. The view has no
// rotation, so only the scale+offset part can be folded: a (rare) rotation on the reference itself becomes a
// small one-time visual jump as it rebases; subject space stays exact. The user's align region lives in
// subject space, so it is rebased along, keeping it glued to its content.
AlignmentTransform PhotoCompareWindow::rebaseSubjectSpaceToReference()
{
	Photo& ref = _photos[_refIndex];
	const AlignmentTransform oldTransform{ ref.alignScale, ref.alignRotation, ref.alignOffset };
	_viewPan += _viewZoom * ref.alignOffset;
	_viewZoom *= ref.alignScale;
	if (!_alignAoi.isEmpty())
		_alignAoi = subjectRectToImage(_alignAoi, oldTransform.scale, oldTransform.rotation, oldTransform.offset);
	ref.alignScale = 1.0;
	ref.alignRotation = 0.0;
	ref.alignOffset = QPointF();
	return oldTransform;
}

void PhotoCompareWindow::autoAlignPhotos()
{
	if (_photos.size() < 2)
		return;
	if (_calibrating)
		setCalibrating(false);
	QApplication::setOverrideCursor(Qt::BusyCursor);
	for (Photo& photo : _photos)
	{
		photo.alignMarks.clear();
		photo.alignScored = false;  // re-derived below for each non-reference photo; the reference stays unscored
	}

	Photo& ref = _photos[_refIndex];
	// The library works in the reference's PIXEL space - rebase subject space onto it. From here on each
	// photo's current mapping serves both as the initial guess and as the kept alignment when the aligner
	// reports failure, and the rebased _alignAoi is directly usable as the library's areaOfInterest.
	const AlignmentTransform refTransform = rebaseSubjectSpaceToReference();

	// Non-reference photos align against the reference independently, so run them across the window pool. Each
	// task writes only its own _photos[targets[k]] and its own contrib[k]; the reference-side marks (every
	// photo appends to the single ref.alignMarks) and _alignMarkSize are merged on this thread after the
	// region, in photo order, so the outcome is bit-identical to a serial run. alignImages still receives the
	// pool: nesting parallelFor on it is deadlock-free and self-balancing - few photos let each inner fit use
	// the spare cores, many photos saturate the pool from the outer loop so each fit then runs mostly inline.
	const bool fitRotation = !_ignoreRotationCheck->isChecked();  // hoisted: no widget access off the GUI thread

	std::vector<size_t> targets;  // the non-reference photos, in order
	for (size_t i = 0; i < _photos.size(); ++i)
		if (static_cast<int>(i) != _refIndex)
			targets.push_back(i);

	struct RefContribution { std::vector<AlignmentMark> refMarks; double patchSize = 0.0; };
	std::vector<RefContribution> contrib(targets.size());  // one slot per task, merged serially below

	_workerPool.parallelFor(targets.size(), [&](size_t k) {
		Photo& photo = _photos[targets[k]];
		AlignmentOptions options;
		options.areaOfInterest = _alignAoi;  // empty = whole frame
		options.fitRotation = fitRotation;
		// refTransform^-1 * photoTransform: the photo's mapping into the rebased subject space.
		options.initialGuess = { photo.alignScale / refTransform.scale, photo.alignRotation - refTransform.rotation,
		                         rotated(photo.alignOffset - refTransform.offset, -refTransform.rotation) / refTransform.scale };
		QElapsedTimer alignTimer;
		alignTimer.start();
		const AlignmentResult result = alignImages(ref.image, photo.image, options, &_workerPool);
		photo.alignTimeMs = alignTimer.nsecsElapsed() / 1e6;  // wall time; under the parallel run it reflects core contention
		contrib[k].patchSize = result.patchSize;
		photo.alignConfidence = result.confidence;        // all surfaced in this photo's corner caption, success or not
		photo.alignBootstrapZncc = result.bootstrapZncc;
		photo.alignRotationSigma = result.rotationSigma;
		photo.alignSucceeded = result.succeeded;
		photo.alignScored = true;
		for (const AlignmentPatchInfo& patchInfo : result.patches)
		{
			const auto kind = patchInfo.fate == AlignmentPatchFate::Accepted ? AlignmentMark::Kind::Used
			                : patchInfo.fate == AlignmentPatchFate::AcceptedCoarse ? AlignmentMark::Kind::UsedCoarse
			                : patchInfo.fate == AlignmentPatchFate::Outlier ? AlignmentMark::Kind::Outlier
			                                                                : AlignmentMark::Kind::Failed;
			contrib[k].refMarks.push_back({ patchInfo.refPoint, kind });
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
	});

	// Merge each photo's reference-side marks in photo order (identical to the serial accumulation).
	for (const RefContribution& c : contrib)
		ref.alignMarks.insert(ref.alignMarks.end(), c.refMarks.begin(), c.refMarks.end());
	_alignMarkSize = contrib.front().patchSize;  // every call returns the same patchSize; in reference px == subject units
	QApplication::restoreOverrideCursor();
	updateAllPanes();
}

Qt::CursorShape PhotoCompareWindow::idleCursor() const
{
	return _calibrating ? Qt::CrossCursor : Qt::OpenHandCursor;
}

void PhotoCompareWindow::setCalibrating(bool calibrating)
{
	_calibrating = calibrating;
	_slider->setEnabled(!calibrating);  // the full view has no per-pane clicks, so it is off-limits mid-calibration
	// Calibration clicks map to the pane's own photo, so a still-held flicker override (panes showing some
	// other photo) would have the user placing points against the wrong picture - drop it.
	_flickerIndex = -1;
	for (Photo& photo : _photos)
	{
		photo.calibPoints.clear();
		photo.alignMarks.clear();  // stale auto-align circles would only be clutter under the crosshairs
		photo.alignScored = false;  // and the auto-align scores they went with
	}
	for (PhotoComparePane* paneWidget : _paneWidgets)
		paneWidget->setCursor(idleCursor());
	updateHintText();
	updateAllPanes();
}

void PhotoCompareWindow::addCalibrationPoint(int index, const QPointF& imagePos)
{
	auto& points = _photos[index].calibPoints;
	if (points.size() >= 2)
		return;
	// A near-duplicate would make the two-point distance ratio meaningless (or divide by ~zero) - ignore it.
	if (points.size() == 1 && QLineF(points[0], imagePos).length() < 4.0)
		return;
	// The pane receiving the session's very first point becomes the reference the others are mapped onto.
	if (std::all_of(_photos.cbegin(), _photos.cend(), [](const Photo& photo) { return photo.calibPoints.empty(); }))
		_refIndex = index;
	points.push_back(imagePos);
	updateHintText();
	_paneWidgets[index]->update();

	const bool allPlaced = std::all_of(_photos.cbegin(), _photos.cend(),
	                                   [](const Photo& photo) { return photo.calibPoints.size() == 2; });
	if (allPlaced)
		applyCalibration();
}

void PhotoCompareWindow::undoCalibrationPoint(int index)
{
	auto& points = _photos[index].calibPoints;
	if (points.empty())
		return;
	points.pop_back();
	updateHintText();
	_paneWidgets[index]->update();
}

void PhotoCompareWindow::applyCalibration()
{
	// The reference is the pane that received the session's first point: subject space becomes its pixel
	// coords. Every other photo maps onto it by the similarity that carries its two clicked points exactly
	// onto the reference's two: scale = the distance ratio, rotation = the angle between the segments,
	// offset = what maps the midpoints. Two point pairs determine all four parameters, so this handles
	// arbitrary angles - beyond auto-align's small-angle capture range.
	const Photo& ref = _photos[_refIndex];
	const QLineF refLine(ref.calibPoints[0], ref.calibPoints[1]);
	rebaseSubjectSpaceToReference();  // the reference image stays exactly where it was on screen; only the other photos move to meet it
	for (size_t i = 0; i < _photos.size(); ++i)
	{
		if (static_cast<int>(i) == _refIndex)
			continue;
		Photo& photo = _photos[i];
		const QLineF line(photo.calibPoints[0], photo.calibPoints[1]);
		photo.alignScale = refLine.length() / line.length();
		// std::remainder wraps the atan2 difference back into [-Pi, Pi]
		const double rotation = std::remainder(std::atan2(refLine.dy(), refLine.dx()) - std::atan2(line.dy(), line.dx()), 2.0 * std::numbers::pi);
		photo.alignRotation = rotation;
		photo.alignOffset = refLine.center() - photo.alignScale * rotated(line.center(), rotation);
	}
	setCalibrating(false);  // also repaints all panes
}

void PhotoCompareWindow::setFullViewIndex(int index)
{
	const bool entering = _fullViewIndex < 0;
	_fullViewIndex = index;
	_slider->setValue(index);  // no-op when the slider itself is the source
	if (entering)  // the viewport grows from one grid cell to the whole stack area
		switchViewportPage(1, _paneWidgets[0]->size(), _viewStack->widget(0)->size());
	else
	{
		updateHintText();  // the "N/M" position readout
		updateAllPanes();
	}
}

void PhotoCompareWindow::exitFullView()
{
	if (_fullViewIndex < 0)
		return;
	_fullViewIndex = -1;
	// Grid panes keep their pre-full-view geometry while hidden, so pane 0's size is the new viewport.
	switchViewportPage(0, _fullPane->size(), _paneWidgets[0]->size());
}

void PhotoCompareWindow::switchViewportPage(int page, const QSizeF& oldViewportSize, const QSizeF& newViewportSize)
{
	_viewStack->setCurrentIndex(page);
	// A touched view keeps the subject point at the viewport's center fixed (both sizes were read before the
	// switch); an untouched one re-fits to the new viewport as it would on any resize.
	if (_viewTouched)
		_viewPan += QPointF(newViewportSize.width() - oldViewportSize.width(), newViewportSize.height() - oldViewportSize.height()) / 2.0;
	else
		fitView();  // a stale new-pane size here self-corrects: the layout resizes it -> onPaneResized -> re-fit
	updateHintText();
	updateAllPanes();
}

void PhotoCompareWindow::setDifferenceMode(bool difference)
{
	_differenceMode = difference;
	_diffToggle->setCurrentIndex(difference ? 1 : 0);  // silent; a no-op when the toggle itself is the source
	updateAllPanes();
}

void PhotoCompareWindow::onPaneResized()
{
	// Until the user navigates, every layout change (first show, maximize, a later window resize) re-fits
	// the view; after that the view is theirs and resizes leave it alone.
	if (!_viewTouched)
		fitView();
}

void PhotoCompareWindow::updateAllPanes()
{
	for (PhotoComparePane* paneWidget : _paneWidgets)
		paneWidget->update();
	if (_fullPane)
		_fullPane->update();
}

void PhotoCompareWindow::updateHintText()
{
	if (_loadBatch)
		_hintLabel->setText(tr("Loading photos... %1 of %2").arg(_loadBatch->completedCount.load()).arg(_loadBatch->paths.size()));
	else if (_photos.empty())
		_hintLabel->setText(tr("Drop images or folders onto the window to load them · Esc: close"));
	else if (_calibrating)
	{
		QStringList progress;
		for (size_t i = 0; i < _photos.size(); ++i)
		{
			QString entry = QString("%1: %2/2").arg(i + 1).arg(_photos[i].calibPoints.size());
			if (static_cast<int>(i) == _refIndex && !_photos[i].calibPoints.empty())
				entry += tr(" (ref)");
			progress.push_back(entry);
		}
		_hintLabel->setText(tr("Alignment: click the same two features in every photo · right-click: undo a point · Esc: cancel · %1")
			.arg(progress.join("   ")));
	}
	else if (_fullViewIndex >= 0)
		_hintLabel->setText(tr("Full view %1/%2 · slider / Left,Right: switch photo · hold 1..%2: flicker · A: auto-align · Shift+drag: align region · D: difference · I: patch info · wheel: zoom · drag: pan · Ctrl+wheel / Ctrl+drag: adjust this photo · F / Ctrl+0 / Home: fit · Ctrl+1: actual px · R: reset · Esc: back to grid")
			.arg(_fullViewIndex + 1).arg(_photos.size()));
	else
		_hintLabel->setText(tr("Wheel: zoom · drag: pan · Ctrl+wheel / Ctrl+drag: adjust one photo · A: auto-align · Shift+A: align by 2 clicks · Shift+drag: align region · hold 1..%1: flicker · D: difference · I: patch info · slider: full view · F / Ctrl+0 / Home / double-click: fit · Ctrl+1: actual px · R: reset · Esc: close")
			.arg(_photos.size()));
}
