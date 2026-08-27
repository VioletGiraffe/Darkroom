#include "Windows/PhotoCompareWindow.h"
#include "Core/Catalog.h"
#include "Core/IoThreadPool.h"
#include "Core/Library.h"
#include "MagicAlignment.h"
#include "Theme/Theme.h"
#include "UiComponents/MarkerSlider.h"
#include "UiComponents/SegmentedToggle.h"
#include "Utils.h"
#include "Windows/MediaItemManagement.h"

#include "assert/advanced_assert.h"
#include "compiler/compiler_warnings_control.h"
#include "utils/naturalsorting/cnaturalsorterqcollator.h"

DISABLE_COMPILER_WARNINGS
#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QContextMenuEvent>
#include <QDebug>
#include <QDir>
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
#include <QSignalBlocker>
#include <QSlider>
#include <QStackedLayout>
#include <QtMath>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>
RESTORE_COMPILER_WARNINGS

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <numbers>
#include <optional>
#include <thread>
#include <utility>

namespace Settings {
	constexpr const char* PhotoCompareIgnoreRotation = "photoCompare/ignoreRotation";
	constexpr const char* PhotoCompareAoi = "photoCompare/aoiNormalized";
}

namespace {

// Each photo retains a full-resolution image and mipmap chain.
constexpr qsizetype MaxImages = 50;

QPointF rotated(const QPointF& p, double angle)
{
	const double c = std::cos(angle), s = std::sin(angle);
	return QPointF(c * p.x() - s * p.y(), s * p.x() + c * p.y());
}

QRectF subjectRectToImage(const QRectF& rect, double scale, double rotation, const QPointF& offset)
{
	QPolygonF corners;
	for (const QPointF& corner : { rect.topLeft(), rect.topRight(), rect.bottomLeft(), rect.bottomRight() })
		corners.push_back(rotated(corner - offset, -rotation) / scale);
	return corners.boundingRect();
}

} // namespace

// Shared by the GUI, I/O, and decode stages of one load.
struct PhotoCompareWindow::PhotoLoadBatch
{
	QStringList paths;
	std::vector<QImage> images;
	QString completionNotice;
	std::atomic<int> completedCount{ 0 };
	std::atomic<bool> abort{ false };
};

// A pane is either a grid cell or, at index -1, the full-view viewport.
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
	[[nodiscard]] int photoIndex() const { return _index >= 0 ? _index : _owner._fullViewIndex; }

	void drawCaption(QPainter& painter, const PhotoCompareWindow::Photo& photo, int renderIndex) const;

	PhotoCompareWindow& _owner;
	const int _index;

	QPointF _pressPos;
	QPointF _lastDragPos;
	bool _leftButtonDown = false;
	bool _dragConfirmed = false;
	bool _ctrlDrag = false;

	// Subject-space anchoring keeps the selection attached to content while the view moves.
	bool _aoiDrag = false;
	QPointF _aoiAnchor;
};

void PhotoComparePane::paintEvent(QPaintEvent*)
{
	QPainter painter(this);
	painter.fillRect(rect(), Theme::current().thumbnailMatte);

	const int renderIndex = _owner._flickerIndex >= 0 ? _owner._flickerIndex : photoIndex();
	PhotoCompareWindow::Photo& photo = _owner._photos[renderIndex];

	const auto drawPhoto = [&](PhotoCompareWindow::Photo& drawn) {
		const double drawnScale = _owner._viewZoom * drawn.alignScale;
		double residualScale = 1.0;
		const QImage& source = _owner.imageForScale(drawn, drawnScale, devicePixelRatioF(), residualScale);
		painter.save();
		painter.setRenderHint(QPainter::SmoothPixmapTransform);
		painter.translate(_owner._viewZoom * drawn.alignOffset + _owner._viewPan);
		painter.rotate(qRadiansToDegrees(drawn.alignRotation));
		painter.scale(residualScale, residualScale);
		painter.drawImage(0, 0, source);
		painter.restore();
	};

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

	if (_owner._calibrating)
	{
		painter.setPen(QPen(Theme::current().palette.accent, 2));
		for (const QPointF& imagePos : photo.calibPoints)
		{
			const QPointF c = _owner.widgetFromImage(photo, imagePos);
			painter.drawLine(c - QPointF(8, 0), c + QPointF(8, 0));
			painter.drawLine(c - QPointF(0, 8), c + QPointF(0, 8));
		}
	}

	if (_owner._showAlignDiagnostics)
	{
		painter.setBrush(Qt::NoBrush);
		const double markHalf = 0.5 * _owner._alignMarkSize * _owner._viewZoom;
		for (const PhotoCompareWindow::AlignmentMark& mark : photo.alignMarks)
		{
			using Kind = PhotoCompareWindow::AlignmentMark::Kind;
			const QColor color = mark.kind == Kind::Used || mark.kind == Kind::UsedCoarse ? Theme::current().palette.accent
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

	if (!_owner._alignAoi.isEmpty())
	{
		painter.setPen(QPen(QColor(0xe8, 0xe8, 0xe8), 1, Qt::DashLine));
		painter.setBrush(Qt::NoBrush);
		painter.drawRect(QRectF(_owner._viewZoom * _owner._alignAoi.topLeft() + _owner._viewPan,
		                        _owner._alignAoi.size() * _owner._viewZoom));
	}

	drawCaption(painter, photo, renderIndex);

	if (photoIndex() == _owner._refIndex && _owner._photos.size() > 1)
	{
		painter.setPen(QPen(QColor(240, 224, 64), 2.0));
		painter.setBrush(Qt::NoBrush);
		painter.drawRect(rect().adjusted(1, 1, -1, -1));
	}

	if (renderIndex != photoIndex())
	{
		painter.setPen(QPen(Theme::current().palette.accent, 3));
		painter.setBrush(Qt::NoBrush);
		painter.drawRect(rect().adjusted(1, 1, -2, -2));
	}
}

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
		if (!photo.alignSucceeded)
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
	if (_owner._calibrating)
		mousePressEvent(event);
	else if (event->button() == Qt::LeftButton)
		_owner.fitView();
}

void PhotoComparePane::contextMenuEvent(QContextMenuEvent* event)
{
	if (_owner._calibrating)
		return;
	const int index = photoIndex();
	QMenu menu;
	menu.addAction(revealInFileManagerActionText(), [this, index] {
		if (const QString path = _owner._photos[index].filePath; !revealInFileManager(path))
			reportMissingFile(this, path);
	});
	QAction* makeReference = menu.addAction(tr("Make this the reference image"), [this, index] {
		_owner._refIndex = index;
		_owner.updateAllPanes();
	});
	makeReference->setEnabled(index != _owner._refIndex);
	menu.addSeparator();
	menu.addAction(tr("Delete this photo from disk"), [this, index] { _owner.deletePhotoInteractive(index); });
	menu.exec(event->globalPos());
}

void PhotoCompareWindow::showForFiles(Library& library, const QStringList& candidatePaths, QWidget* parent,
	PhotoRemovedHandler photoRemovedHandler)
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

	auto* w = new PhotoCompareWindow(library, paths, parent, std::move(photoRemovedHandler));
	w->setAttribute(Qt::WA_DeleteOnClose);
	w->show();
}

PhotoCompareWindow::PhotoCompareWindow(Library& library, const QStringList& photoPaths, QWidget* parent,
	PhotoRemovedHandler photoRemovedHandler) : QWidget(parent, Qt::Window),
	_library(library),
	_photoRemovedHandler(std::move(photoRemovedHandler)),
	_workerPool(std::max(std::thread::hardware_concurrency(), 2u) - 1, "photo-compare")
{
	setWindowTitle(tr("Compare Photos"));
	setFocusPolicy(Qt::StrongFocus);
	setAcceptDrops(true);

	QVBoxLayout* mainLayout = new QVBoxLayout(this);
	mainLayout->setContentsMargins(4, 4, 4, 4);

	_gridPage = new QWidget(this);
	_dropHintLabel = new QLabel(_gridPage);
	_dropHintLabel->setAlignment(Qt::AlignCenter);

	_fullPane = new PhotoComparePane(*this, -1);
	_viewStack = new QStackedLayout();
	_viewStack->addWidget(_gridPage);
	_viewStack->addWidget(_fullPane);
	mainLayout->addLayout(_viewStack, 1);

	QHBoxLayout* toolbar = new QHBoxLayout();

	_slider = new MarkerSlider(Qt::Horizontal, this);
	_slider->setPageStep(1);
	_slider->setTickPosition(QSlider::TicksBelow);
	_slider->setTickInterval(1);
	_slider->setFocusPolicy(Qt::NoFocus);
	connect(_slider, &QSlider::sliderPressed, this, [this] { setFullViewIndex(_slider->value()); });
	connect(_slider, &QSlider::valueChanged, this, [this](int value) { setFullViewIndex(value); });
	toolbar->addWidget(_slider, 1);

	_ignoreRotationCheck = new QCheckBox(tr("Ignore rotation"), this);
	_ignoreRotationCheck->setToolTip(tr("Auto-align fits scale and offset only, treating any apparent rotation as spurious\n"
	                                     "(e.g. depth parallax between focus-stack slices can read as a slight tilt)"));
	_ignoreRotationCheck->setFocusPolicy(Qt::NoFocus);
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

	addPhotosFromFiles(photoPaths);

	if (!restoreWindowGeometry(this, "photoCompareWindow"))
	{
		resize(1200, 800);
		setWindowState(Qt::WindowMaximized);
	}
}

PhotoCompareWindow::~PhotoCompareWindow()
{
	if (_loadBatch)
	{
		_loadBatch->abort = true;
		// retire() prevents the I/O task from touching this window or enqueueing more decode work.
		IoThreadPool::retire(reinterpret_cast<uint64_t>(this));
	}
	saveWindowGeometry(this, "photoCompareWindow");
	// Persist the region relative to the reference resolution.
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
	// Reference rotation is zero after every supported alignment path.
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
			setDefaultAlignment(photo);
	}
	_alignAoi = QRectF();
	_viewTouched = false;
	fitView();
	updateHintText();
	updateAllPanes();
}

void PhotoCompareWindow::addPhotosFromFiles(const QStringList& photoPaths)
{
	assert_debug_only(!_loadBatch);
	if (photoPaths.isEmpty())
	{
		PhotoLoadBatch emptyBatch;
		applyLoadedPhotoBatch(emptyBatch);
		return;
	}

	auto batch = std::make_shared<PhotoLoadBatch>();
	batch->paths = photoPaths;
	batch->images.resize(static_cast<size_t>(photoPaths.size()));
	_loadBatch = batch;
	if (_photos.empty())
	{
		rebuildPaneGrid();
		_slider->setEnabled(false);
	}
	updateHintText();

	// Serialize reads per storage device, then decode each completed read in the compute pool.
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
			_workerPool.enqueue([this, batch, i, fileBytes = std::move(fileBytes)]() mutable {
				if (!batch->abort)
				{
					QBuffer buffer(&fileBytes);
					QImageReader reader(&buffer);  // A format hint would suppress content detection.
					reader.setAutoTransform(true);
					QImage& image = batch->images[static_cast<size_t>(i)];
					image = reader.read();
					if (image.isNull())
						qWarning() << "PhotoCompareWindow: failed to decode" << batch->paths[i] << "-" << reader.errorString();
				}
				if (++batch->completedCount == static_cast<int>(batch->paths.size()))
					QMetaObject::invokeMethod(this, [this, batch] { applyLoadedPhotoBatch(*batch); }, Qt::QueuedConnection);
				else
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
			continue;
		Photo photo;
		photo.image = std::move(image);
		photo.filePath = batch.paths[i];
		photo.caption = QFileInfo(batch.paths[i]).fileName();
		// Normalize height so resolution-only differences align by default.
		if (!_photos.empty())
			setDefaultAlignment(photo);
		_photos.push_back(std::move(photo));
	}

	rebuildPaneGrid();
	_slider->setRange(0, std::max(0, static_cast<int>(_photos.size()) - 1));
	_slider->setEnabled(!_photos.empty() && !_calibrating);
	if (!_viewTouched)
		fitView();
	// Restore the normalized region only once a reference exists.
	if (oldCount == 0 && !_photos.empty() && _alignAoi.isEmpty())
	{
		const QRectF normalizedAoi = QSettings{}.value(Settings::PhotoCompareAoi).toRectF();
		if (!normalizedAoi.isEmpty())
			_alignAoi = subjectRectFromNormalized(normalizedAoi);
	}
	updateHintText();
	updateAllPanes();
	if (_photos.size() == oldCount && !batch.paths.isEmpty())
		_hintLabel->setText(tr("None of the files could be loaded as images."));
	else if (!batch.completionNotice.isEmpty())
		_hintLabel->setText(batch.completionNotice);
}

void PhotoCompareWindow::rebuildPaneGrid()
{
	delete _gridPage->layout();
	QGridLayout* grid = new QGridLayout(_gridPage);
	grid->setContentsMargins(0, 0, 0, 0);
	grid->setSpacing(4);

	const int photoCount = static_cast<int>(_photos.size());
	for (PhotoComparePane* pane : _paneWidgets)
		pane->hide();
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
	{
		grid->addWidget(_paneWidgets[i], i / columns, i % columns);
		_paneWidgets[i]->show();
	}
	// Shared widget-space pan requires equally sized panes.
	for (int c = 0; c < columns; ++c)
		grid->setColumnStretch(c, 1);
	for (int r = 0; r < rows; ++r)
		grid->setRowStretch(r, 1);
}

void PhotoCompareWindow::deletePhotoInteractive(int index)
{
	if (index < 0 || index >= static_cast<int>(_photos.size()))
	{
		assert_unconditional_r("Invalid photo-comparison index");
		return;
	}

	const QString filePath = _photos[index].filePath;
	const QString pathKey = pathComparisonKey(filePath);
	std::optional<MediaId> trackedId;
	Catalog& catalog = _library.catalog();
	for (auto it = catalog.mediaItems().cbegin(); it != catalog.mediaItems().cend(); ++it)
	{
		if (it->type == Catalog::MediaType::Photo && pathComparisonKey(it->sourcePath) == pathKey)
		{
			trackedId = it.key();
			break;
		}
	}

	if (trackedId)
	{
		const MediaItemManagement::DeleteResult result = MediaItemManagement::deleteItemsInteractive(catalog, { *trackedId }, this);
		if (std::ranges::contains(result.deletedItems, *trackedId))
			removePhotoFromComparison(index);
		return;
	}

	const QString question = tr("Move this photo to Trash?\n\n%1").arg(QDir::toNativeSeparators(filePath));
	if (QMessageBox::warning(this, tr("Delete photo"), question,
			QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
		return;

	if (MediaItemManagement::removePathTrashFirstInteractive(filePath, this))
		removePhotoFromComparison(index);
}

void PhotoCompareWindow::removePhotoFromComparison(int index)
{
	const QString removedPath = _photos[index].filePath;
	const bool removedReference = index == _refIndex;
	exitFullView();
	_flickerIndex = -1;
	_photos.erase(_photos.begin() + index);
	if (_photos.empty())
	{
		_refIndex = 0;
		_alignAoi = QRectF();
		_viewTouched = false;
	}
	else if (removedReference)
		_refIndex = std::min(index, static_cast<int>(_photos.size()) - 1);
	else if (index < _refIndex)
		--_refIndex;

	rebuildPaneGrid();
	{
		const QSignalBlocker blocker(_slider);
		_slider->setRange(0, std::max(0, static_cast<int>(_photos.size()) - 1));
		_slider->setValue(std::min(_slider->value(), _slider->maximum()));
	}
	_slider->setEnabled(!_photos.empty() && !_calibrating);
	if (removedReference && !_photos.empty())
		resetToInitialState();
	else
	{
		if (!_viewTouched)
			fitView();
		updateHintText();
		updateAllPanes();
	}

	if (_photoRemovedHandler)
		_photoRemovedHandler(removedPath);
}

void PhotoCompareWindow::dragEnterEvent(QDragEnterEvent* event)
{
	if (_loadBatch)
		return;
	const QList<QUrl> urls = event->mimeData()->urls();
	if (std::any_of(urls.cbegin(), urls.cend(), [](const QUrl& url) { return url.isLocalFile(); }))
		event->acceptProposedAction();
}

void PhotoCompareWindow::dropEvent(QDropEvent* event)
{
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
			_loadBatch->completionNotice = notice;
		else
			_hintLabel->setText(notice);
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
			exitFullView();
			setCalibrating(!_calibrating);
		}
		else
			autoAlignPhotos();
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
		_slider->setValue(_slider->value() + (key == Qt::Key_Right ? 1 : -1));
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
	// Select mipmaps in physical pixels; logical scale alone is too coarse on HiDPI.
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
			break;
		photo.mipmaps.push_back(prev.scaled((prev.width() + 1) / 2, (prev.height() + 1) / 2,
		                                    Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
	}
	level = std::min(level, static_cast<int>(photo.mipmaps.size()));
	residualScale = effectiveScale * std::pow(2.0, level);
	return level == 0 ? photo.image : photo.mipmaps[level - 1];
}

void PhotoCompareWindow::zoomView(double factor, const QPointF& widgetAnchor)
{
	const double newZoom = std::clamp(_viewZoom * factor, 0.01, 100.0);
	const double applied = newZoom / _viewZoom;
	_viewZoom = newZoom;
	_viewPan = widgetAnchor - applied * (widgetAnchor - _viewPan);
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
	// Actual pixels means physical, not Qt logical, pixels.
	const double targetZoom = 1.0 / (pane->devicePixelRatioF() * _photos[_refIndex].alignScale);
	zoomView(targetZoom / _viewZoom, QPointF(paneSize.width() / 2.0, paneSize.height() / 2.0));
}

void PhotoCompareWindow::adjustPhotoScale(int index, double factor, const QPointF& widgetAnchor)
{
	Photo& photo = _photos[index];
	const QPointF subjectAnchor = subjectFromWidget(widgetAnchor);
	const QPointF imageAnchor = imageFromWidget(photo, widgetAnchor);
	photo.alignScale = std::clamp(photo.alignScale * factor, 0.01, 100.0);
	photo.alignOffset = subjectAnchor - photo.alignScale * rotated(imageAnchor, photo.alignRotation);
	_viewTouched = true;
	updateAllPanes();
}

void PhotoCompareWindow::movePhotoOffset(int index, const QPointF& widgetDelta)
{
	_photos[index].alignOffset += widgetDelta / _viewZoom;
	_viewTouched = true;
	updateAllPanes();
}

// Rebase subject space to reference pixels while preserving its on-screen scale and offset.
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
		photo.alignScored = false;
	}

	Photo& ref = _photos[_refIndex];
	const AlignmentTransform refTransform = rebaseSubjectSpaceToReference();

	// Workers write disjoint photos; reference-side diagnostics are merged serially below.
	const bool fitRotation = !_ignoreRotationCheck->isChecked();  // Do not access widgets from workers.

	std::vector<size_t> targets;
	for (size_t i = 0; i < _photos.size(); ++i)
		if (static_cast<int>(i) != _refIndex)
			targets.push_back(i);

	struct RefContribution { std::vector<AlignmentMark> refMarks; double patchSize = 0.0; };
	std::vector<RefContribution> contrib(targets.size());

	_workerPool.parallelFor(targets.size(), [&](size_t k) {
		Photo& photo = _photos[targets[k]];
		AlignmentOptions options;
		options.areaOfInterest = _alignAoi;
		options.fitRotation = fitRotation;
		// refTransform^-1 * photoTransform.
		options.initialGuess = { photo.alignScale / refTransform.scale, photo.alignRotation - refTransform.rotation,
		                         rotated(photo.alignOffset - refTransform.offset, -refTransform.rotation) / refTransform.scale };
		QElapsedTimer alignTimer;
		alignTimer.start();
		const AlignmentResult result = alignImages(ref.image, photo.image, options, &_workerPool);
		photo.alignTimeMs = alignTimer.nsecsElapsed() / 1e6;
		contrib[k].patchSize = result.patchSize;
		photo.alignConfidence = result.confidence;
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
			if (patchInfo.zncc > 0.0)  // targetPoint is undefined when no match was found.
				photo.alignMarks.push_back({ patchInfo.targetPoint, kind });
		}
		if (result.succeeded)
		{
			photo.alignScale = result.transform.scale;
			photo.alignRotation = result.transform.rotation;
			photo.alignOffset = result.transform.offset;
		}
		else
		{
			photo.alignScale = options.initialGuess.scale;
			photo.alignRotation = options.initialGuess.rotation;
			photo.alignOffset = options.initialGuess.offset;
		}
	});

	for (const RefContribution& c : contrib)
		ref.alignMarks.insert(ref.alignMarks.end(), c.refMarks.begin(), c.refMarks.end());
	_alignMarkSize = contrib.front().patchSize;
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
	_slider->setEnabled(!calibrating);
	// Calibration clicks must correspond to each pane's own photo.
	_flickerIndex = -1;
	for (Photo& photo : _photos)
	{
		photo.calibPoints.clear();
		photo.alignMarks.clear();
		photo.alignScored = false;
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
	// Near-duplicates make the two-point distance ratio unstable.
	if (points.size() == 1 && QLineF(points[0], imagePos).length() < 4.0)
		return;
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
	// Two point pairs determine the scale, rotation, and offset similarity exactly.
	const Photo& ref = _photos[_refIndex];
	const QLineF refLine(ref.calibPoints[0], ref.calibPoints[1]);
	rebaseSubjectSpaceToReference();
	for (size_t i = 0; i < _photos.size(); ++i)
	{
		if (static_cast<int>(i) == _refIndex)
			continue;
		Photo& photo = _photos[i];
		const QLineF line(photo.calibPoints[0], photo.calibPoints[1]);
		photo.alignScale = refLine.length() / line.length();
		const double rotation = std::remainder(std::atan2(refLine.dy(), refLine.dx()) - std::atan2(line.dy(), line.dx()), 2.0 * std::numbers::pi);
		photo.alignRotation = rotation;
		photo.alignOffset = refLine.center() - photo.alignScale * rotated(line.center(), rotation);
	}
	setCalibrating(false);
}

void PhotoCompareWindow::setFullViewIndex(int index)
{
	const bool entering = _fullViewIndex < 0;
	_fullViewIndex = index;
	_slider->setValue(index);
	if (entering)
		switchViewportPage(1, _paneWidgets[0]->size(), _viewStack->widget(0)->size());
	else
	{
		updateHintText();
		updateAllPanes();
	}
}

void PhotoCompareWindow::exitFullView()
{
	if (_fullViewIndex < 0)
		return;
	_fullViewIndex = -1;
	switchViewportPage(0, _fullPane->size(), _paneWidgets[0]->size());
}

void PhotoCompareWindow::switchViewportPage(int page, const QSizeF& oldViewportSize, const QSizeF& newViewportSize)
{
	_viewStack->setCurrentIndex(page);
	// Preserve the center for a user-positioned view; otherwise keep fitting automatically.
	if (_viewTouched)
		_viewPan += QPointF(newViewportSize.width() - oldViewportSize.width(), newViewportSize.height() - oldViewportSize.height()) / 2.0;
	else
		fitView();
	updateHintText();
	updateAllPanes();
}

void PhotoCompareWindow::setDifferenceMode(bool difference)
{
	_differenceMode = difference;
	_diffToggle->setCurrentIndex(difference ? 1 : 0);
	updateAllPanes();
}

void PhotoCompareWindow::onPaneResized()
{
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
