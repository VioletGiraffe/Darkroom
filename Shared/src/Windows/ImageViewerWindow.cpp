#include "Windows/ImageViewerWindow.h"
#include "Core/CpuThreadPool.h"
#include "ImageDecoding/ImageDecoding.h"
#include "Shortcuts.h"
#include "Utils.h"
#include "assert/advanced_assert.h"
#include "utils/filemanager.h"
#include "widgets/cimageviewerwidget.h"

DISABLE_COMPILER_WARNINGS
#include "resize/qimage_resize.h"

#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QImageWriter>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLayout>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QMimeDatabase>
#include <QMimeType>
#include <QMouseEvent>
#include <QSaveFile>
#include <QSize>
#include <QTimer>
#include <QUrl>
#include <QWheelEvent>
RESTORE_COMPILER_WARNINGS

#include <algorithm>
#include <array>
#include <cstdlib>
#include <functional>
#include <optional>
#include <utility>

constexpr QSize MinViewSize{ 120, 100 };
constexpr int LossyEncodeQuality = 90;

struct SaveAsType
{
	const char* mimeType;
	// Only lossy types take LossyEncodeQuality: the PNG writer maps quality to its compression level.
	bool lossy;
};

// Offered where a writer plugin for it is present.
constexpr std::array SaveAsTypes{
	SaveAsType{ "image/avif", true },
	SaveAsType{ "image/bmp", false },
	SaveAsType{ "image/heic", true },
	SaveAsType{ "image/heif", true },
	SaveAsType{ "image/jpeg", true },
	SaveAsType{ "image/jxl", true },
	SaveAsType{ "image/png", false },
	SaveAsType{ "image/tiff", false },
	SaveAsType{ "image/webp", true },
};

[[nodiscard]] static bool isLossySaveAsType(const QString& mimeType)
{
	const auto type = std::ranges::find_if(SaveAsTypes, [&](const SaveAsType& listed) { return mimeType == QLatin1String(listed.mimeType); });
	assert_and_return_r(type != SaveAsTypes.end(), false);
	return type->lossy;
}

// Formats Qt decodes stay with the viewer widget's own reader, which also plays animations.
[[nodiscard]] static std::optional<CImageViewerWidget::DecodedFile> decodeWithoutQt(const QString& path)
{
	QFile file{ path };
	if (!file.open(QIODevice::ReadOnly) || ImageDecoding::decoderFor(file.peek(ImageDecoding::SignatureSize)) == ImageDecoding::Decoder::Qt)
		return std::nullopt;

	ImageDecoding::DecodedImage decoded = ImageDecoding::decode(ImageDecoding::readEncodedImage(file, ImageDecoding::Intent::Full));
	ImageDecoding::applyOrientation(decoded.image, decoded.orientation);
	return CImageViewerWidget::DecodedFile{ std::move(decoded.image), QString::fromLatin1(decoded.format) };
}

ImageViewerWindow* ImageViewerWindow::showForImages(QStringList imagePaths, int startIndex, QWidget* parent,
	std::function<void(const QString& path)> onImageChanged)
{
	assert_and_return_r(startIndex >= 0 && startIndex < imagePaths.size(), nullptr);

	if (const QString& startPath = imagePaths.at(startIndex); !QFileInfo::exists(startPath))
	{
		reportMissingFile(parent, startPath);
		return nullptr;
	}

	QWidget* const modalOwner = parent && parent->window()->isModal() ? parent->window() : nullptr;
	auto* window = new ImageViewerWindow(std::move(imagePaths), startIndex, modalOwner);
	// Assigned after construction: the initial image is the one the caller already acted on.
	window->_onImageChanged = std::move(onImageChanged);
	window->setAttribute(Qt::WA_DeleteOnClose);
	window->showFullScreen();
	return window;
}

void ImageViewerWindow::setExitFullScreenHandler(std::function<bool()> handler)
{
	_exitFullScreenHandler = std::move(handler);
}

void ImageViewerWindow::addImageAction(QAction* action, std::function<void(const QString& path)> updateForImage)
{
	_imageMenu->addAction(action);
	addAction(action); // for the shortcut, see buildMenus
	updateForImage(currentImagePath());
	_imageActionUpdaters.push_back(std::move(updateForImage));
}

void ImageViewerWindow::setRemovalActions(RemovalAction onDelete, RemovalAction onShiftDelete)
{
	_imageMenu->addSeparator();
	const auto addRemovalAction = [this](RemovalAction removal, const char* shortcut) {
		QAction* action = _imageMenu->addAction(removal.text, this, [this, remove = std::move(removal.remove)] { removeCurrentImage(remove); });
		action->setShortcut(QKeySequence(shortcut));
		addAction(action); // for the shortcut, see buildMenus
	};
	addRemovalAction(std::move(onDelete), Shortcuts::RemoveFromList);
	addRemovalAction(std::move(onShiftDelete), Shortcuts::DeleteFile);
}

std::vector<ImageViewerWindow*> ImageViewerWindow::_instances;

void ImageViewerWindow::releaseFilesMatching(const std::function<bool(const QString& path)>& isAffected)
{
	for (ImageViewerWindow* viewer : _instances)
	{
		if (viewer->_view->isAnimated() && isAffected(viewer->currentImagePath()))
		{
			viewer->_view->closeFile();
			viewer->_animationReleased = true;
		}
	}
}

void ImageViewerWindow::followMovedFiles(const std::function<bool(const QString& path)>& wasReleased,
	const std::function<QString(const QString& path)>& newLocation)
{
	for (ImageViewerWindow* viewer : _instances)
	{
		const bool reopen = wasReleased(viewer->currentImagePath());
		for (QString& path : viewer->_imagePaths)
			path = newLocation(path);

		viewer->updateTitleAndCaption();
		if (reopen)
			viewer->resumeReleasedAnimation();
	}
}

void ImageViewerWindow::resumeReleasedAnimation()
{
	if (!_animationReleased)
		return;

	_animationReleased = false;
	_view->displayImage(currentImagePath(), false);
}

ImageViewerWindow::ImageViewerWindow(QStringList imagePaths, int startIndex, QWidget* parent)
	: QMainWindow(parent)
	, _imagePaths(std::move(imagePaths))
	, _index(startIndex)
{
	_instances.push_back(this);

	_view = new CImageViewerWidget(this);
	_view->setMinimumSize(MinViewSize);
	setCentralWidget(_view);
	_view->installEventFilter(this);

	_view->setImageScaler([](QImage& dest, const QImage& source, const QRect& srcRect, Qt::TransformationMode mode) {
		const auto kernel = mode == Qt::FastTransformation ? ImageProcessing::ResizeKernel::NearestNeighbor : ImageProcessing::ResizeKernel::Auto;
		if (!ImageProcessing::resize(dest, source, srcRect, cpuParallelFor, kernel))
			CImageViewerWidget::scaleWithQt(dest, source, srcRect, mode);
	});
	_view->setImageDecoder(decodeWithoutQt);

	// The image as the window icon tells several open viewers apart in the taskbar. Restarted per image, so
	// browsing with the arrow keys scales one icon at the end instead of one per image passed through.
	_windowIconTimer = new QTimer(this);
	_windowIconTimer->setSingleShot(true);
	_windowIconTimer->setInterval(3000);
	connect(_windowIconTimer, &QTimer::timeout, this, [this] { setWindowIcon(_view->imageIcon()); });

	buildMenus();
	menuBar()->setVisible(false); // Initially fullscreen - no menu
	showImage(_index);
}

ImageViewerWindow::~ImageViewerWindow()
{
	std::erase(_instances, this);
}

void ImageViewerWindow::buildMenus()
{
	auto* menuBar = new QMenuBar(this);
	setMenuBar(menuBar);

	QMenu* fileMenu = new QMenu(tr("&File"), menuBar);
	fileMenu->addAction(revealInFileManagerActionText(), this, [this] {
		if (!revealInFileManager(currentImagePath()))
			reportMissingFile(this, currentImagePath());
	});
	fileMenu->addAction(tr("Open in system app"), this, [this] {
		QDesktopServices::openUrl(QUrl::fromLocalFile(currentImagePath()));
	});
	QAction* saveAsAction = fileMenu->addAction(tr("Save as..."), this, &ImageViewerWindow::saveImageAs);
	saveAsAction->setShortcut(QKeySequence::SaveAs);
	fileMenu->addSeparator();
	QAction* closeAction = fileMenu->addAction(tr("Close"), this, &QWidget::close);
	closeAction->setShortcuts({ QKeySequence(Qt::Key_Escape), QKeySequence::Close });

	QMenu* editMenu = new QMenu(tr("&Edit"), menuBar);
	QAction* copyAction = editMenu->addAction(tr("Copy image"), this, &ImageViewerWindow::copyImageAndFileToClipboard);
	copyAction->setShortcut(QKeySequence::Copy);

	QMenu* viewMenu = new QMenu(tr("&View"), menuBar);
	QAction* fitAction = viewMenu->addAction(tr("Fit to window"), this, [this] { _view->fitToWindow(); });
	fitAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+0")));
	QAction* actualPixelsAction = viewMenu->addAction(tr("Actual pixels"), this, [this] { _view->zoomToActualPixels(); });
	actualPixelsAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+1")));
	viewMenu->addSeparator();
	QAction* fullScreenAction = viewMenu->addAction(tr("Fullscreen"), this, &ImageViewerWindow::toggleFullScreen);
	fullScreenAction->setShortcut(QKeySequence(Qt::Key_F));
	QAction* overlayAction = viewMenu->addAction(tr("Show image info"));
	overlayAction->setCheckable(true);
	overlayAction->setChecked(_view->isOverlayVisible());
	overlayAction->setShortcut(QKeySequence(Qt::Key_I));
	connect(overlayAction, &QAction::toggled, _view, &CImageViewerWidget::setOverlayVisible);
	_view->setInfoStripHint(tr("Press %1 to hide").arg(overlayAction->shortcut().toString(QKeySequence::NativeText)));

	_imageMenu = new QMenu(tr("&Image"), menuBar);
	_previousAction = _imageMenu->addAction(tr("Previous"), this, [this] { showImage(adjacentIndex(Direction::Previous)); });
	_previousAction->setShortcuts({ QKeySequence(Qt::Key_Left), QKeySequence(Qt::Key_PageUp) });
	_nextAction = _imageMenu->addAction(tr("Next"), this, [this] { showImage(adjacentIndex(Direction::Next)); });
	_nextAction->setShortcuts({ QKeySequence(Qt::Key_Right), QKeySequence(Qt::Key_PageDown) });
	_imageMenu->addSeparator(); // before host actions; QMenu collapses it when none follow

	menuBar->addMenu(fileMenu);
	menuBar->addMenu(editMenu);
	menuBar->addMenu(viewMenu);
	menuBar->addMenu(_imageMenu);

	// A shortcut only matches while one of its action's widgets is visible, and the menu bar is hidden in
	// fullscreen, so the window holds every action as well.
	for (const QMenu* menu : { fileMenu, editMenu, viewMenu, _imageMenu })
		for (QAction* action : menu->actions())
			if (!action->isSeparator())
				addAction(action);

	for (const QMenu* menu : { fileMenu, editMenu, viewMenu, _imageMenu })
		connect(menu, &QMenu::aboutToHide, this, &ImageViewerWindow::hideFullScreenMenuBarOnceMenusClose);
}

void ImageViewerWindow::showImage(int index)
{
	if (index < 0)
		return;

	const QString path = _imagePaths.at(index);
	if (!_view->displayImage(path))
	{
		QMessageBox::warning(this, tr("Error"), tr("Failed to load the image:\n%1").arg(QDir::toNativeSeparators(path)));
		return;
	}

	_animationReleased = false;
	_index = index;
	updateTitleAndCaption();
	_windowIconTimer->start();
	_previousAction->setEnabled(adjacentIndex(Direction::Previous) >= 0);
	_nextAction->setEnabled(adjacentIndex(Direction::Next) >= 0);
	for (const auto& updateForImage : _imageActionUpdaters)
		updateForImage(path);

	if (_onImageChanged)
		_onImageChanged(path);
}

void ImageViewerWindow::updateTitleAndCaption()
{
	const QFileInfo file{ currentImagePath() };
	setWindowTitle(file.completeBaseName());
	_view->setInfoStripCaption(QStringLiteral("%1/%2 • %3").arg(_index + 1).arg(_imagePaths.size()).arg(file.fileName()));
}

bool ImageViewerWindow::eventFilter(QObject* watched, QEvent* event)
{
	if (_trackingAltTap)
		trackAltTap(event);

	if (watched == _view && event->type() == QEvent::MouseButtonDblClick && static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton)
	{
		toggleFullScreen();
		// Swallowed: the widget's default double-click handling forwards to mousePressEvent and starts a pan.
		return true;
	}

	return QMainWindow::eventFilter(watched, event);
}

void ImageViewerWindow::keyPressEvent(QKeyEvent* event)
{
	// A native menu bar (macOS) is never hidden.
	if (event->key() == Qt::Key_Alt && event->modifiers() == Qt::AltModifier && !event->isAutoRepeat()
		&& isFullScreen() && menuBar()->isHidden() && !menuBar()->isNativeMenuBar())
	{
		_trackingAltTap = true;
		qApp->installEventFilter(this);
	}

	QMainWindow::keyPressEvent(event);
}

void ImageViewerWindow::trackAltTap(const QEvent* event)
{
	switch (event->type())
	{
	case QEvent::KeyPress:
	case QEvent::KeyRelease:
		if (static_cast<const QKeyEvent*>(event)->key() == Qt::Key_Alt)
		{
			if (event->type() == QEvent::KeyPress)
				return; // auto-repeat

			stopTrackingAltTap();
			menuBar()->setVisible(true);
			layout()->activate(); // the menu pops up from the bar's geometry, which the layout has yet to set
			menuBar()->setActiveAction(menuBar()->actions().front());
			return;
		}
		break;
	case QEvent::MouseButtonPress:
	case QEvent::MouseButtonRelease:
	case QEvent::Wheel:
	case QEvent::FocusIn:
	case QEvent::FocusOut:
	case QEvent::ActivationChange:
	case QEvent::Shortcut:
		break;
	default:
		return;
	}

	stopTrackingAltTap();
}

void ImageViewerWindow::stopTrackingAltTap()
{
	_trackingAltTap = false;
	qApp->removeEventFilter(this);
}

void ImageViewerWindow::hideFullScreenMenuBarOnceMenusClose()
{
	// Deferred: switching menus with the arrow keys closes one before opening the next.
	QTimer::singleShot(0, this, [this] {
		const QList<QAction*> menuActions = menuBar()->actions();
		if (isFullScreen() && std::ranges::none_of(menuActions, [](const QAction* menuAction) { return menuAction->menu()->isVisible(); }))
			menuBar()->setVisible(false);
	});
}

void ImageViewerWindow::wheelEvent(QWheelEvent* event)
{
	// Ctrl+wheel is the view's zoom, also when the view has no image to zoom and passes it up.
	// Momentum events are ignored: a trackpad flick must not keep browsing after the fingers lift.
	if (event->modifiers().testFlag(Qt::ControlModifier) || event->phase() == Qt::ScrollMomentum)
	{
		QMainWindow::wheelEvent(event);
		return;
	}

	_pendingWheelDelta += event->angleDelta().y();
	if (std::abs(_pendingWheelDelta) >= QWheelEvent::DefaultDeltasPerStep)
	{
		showImage(adjacentIndex(_pendingWheelDelta < 0 ? Direction::Next : Direction::Previous));
		_pendingWheelDelta = 0;
	}
	event->accept();
}

void ImageViewerWindow::toggleFullScreen()
{
	if (isFullScreen() && _exitFullScreenHandler)
	{
		// Cleared before the call: it runs once, and the handler may close this window.
		const auto handler = std::exchange(_exitFullScreenHandler, {});
		if (!handler())
			return;
	}

	// The platform applies the state change before the WindowStateChange event that toggles the menu bar, so the
	// transition otherwise lays out and rescales the image twice. Re-enabling repaints once, at the final size.
	setUpdatesEnabled(false);

	if (isFullScreen())
	{
		showNormal();
		menuBar()->setVisible(true);
	}
	else
	{
		menuBar()->setVisible(false);
		showFullScreen();
	}

	// A zero timer, not a queued call: it waits for the native queue to drain, so updates resume at the settled
	// geometry rather than while the platform still has resize messages pending.
	QTimer::singleShot(0, this, [this] { setUpdatesEnabled(true); });
}

int ImageViewerWindow::adjacentIndex(Direction direction) const
{
	const int step = static_cast<int>(direction);
	for (int index = _index + step; index >= 0 && index < _imagePaths.size(); index += step)
	{
		// The list is a snapshot from when the viewer opened; a since-deleted file must not block navigation past it.
		if (QFileInfo::exists(_imagePaths[index]))
			return index;
	}
	return -1;
}

void ImageViewerWindow::copyImageAndFileToClipboard()
{
	auto* mime = new QMimeData();
	if (const QImage& image = _view->sourceImage(); !image.isNull())
		mime->setImageData(image);
	mime->setUrls({ QUrl::fromLocalFile(currentImagePath()) });
	QGuiApplication::clipboard()->setMimeData(mime);
}

void ImageViewerWindow::saveImageAs()
{
	const QFileInfo source{ currentImagePath() };
	const QMimeDatabase mimeDatabase;

	const QList<QByteArray> supportedTypes = QImageWriter::supportedMimeTypes();
	QStringList writableTypes;
	for (const SaveAsType& type : SaveAsTypes)
	{
		if (supportedTypes.contains(type.mimeType))
			writableTypes.push_back(QString::fromLatin1(type.mimeType));
	}

	// Empty when unlisted. Compares resolved types: a platform's MIME database may know a listed name as an alias.
	const auto listedTypeOf = [&](const QMimeType& type) {
		const auto listed = std::ranges::find_if(writableTypes, [&](const QString& name) { return mimeDatabase.mimeTypeForName(name) == type; });
		return listed != writableTypes.end() ? *listed : QString{};
	};

	// HEIF is a filter only without HEIC, which writes the same files under the conventional extension.
	QStringList filterTypes = writableTypes;
	if (filterTypes.contains(QLatin1String("image/heic")))
		filterTypes.removeOne(QStringLiteral("image/heif"));

	const QString sourceType = listedTypeOf(mimeDatabase.mimeTypeForFile(source));
	QFileDialog dialog(this, tr("Save image as"), source.absolutePath());
	dialog.setAcceptMode(QFileDialog::AcceptSave);
	dialog.setMimeTypeFilters(filterTypes);
	dialog.selectMimeTypeFilter(filterTypes.contains(sourceType) ? sourceType : QStringLiteral("image/png"));
	dialog.selectFile(source.completeBaseName());
	if (dialog.exec() != QDialog::Accepted)
		return;

	QString targetPath = dialog.selectedFiles().front();
	QString targetType = listedTypeOf(mimeDatabase.mimeTypeForFile(targetPath, QMimeDatabase::MatchExtension));
	if (targetType.isEmpty())
	{
		// The dialog checked for overwriting only the name as typed.
		targetType = dialog.selectedMimeTypeFilter();
		targetPath += '.' + mimeDatabase.mimeTypeForName(targetType).preferredSuffix();
		if (QFileInfo::exists(targetPath)
			&& QMessageBox::question(this, dialog.windowTitle(), tr("%1 already exists.\nReplace it?").arg(QDir::toNativeSeparators(targetPath))) != QMessageBox::Yes)
			return;
	}

	if (const QString error = writeCurrentImage(targetPath, targetType, targetType == sourceType); !error.isEmpty())
		QMessageBox::warning(this, tr("Error"), tr("Failed to save the image:\n%1\n\n%2").arg(QDir::toNativeSeparators(targetPath), error));
}

QString ImageViewerWindow::writeCurrentImage(const QString& targetPath, const QString& mimeType, bool copyFile) const
{
	QSaveFile target{ targetPath };
	if (!target.open(QIODevice::WriteOnly))
		return target.errorString();

	if (copyFile)
	{
		QFile source{ currentImagePath() };
		if (!source.open(QIODevice::ReadOnly))
			return source.errorString();

		const QByteArray bytes = source.readAll();
		if (bytes.size() != source.size())
			return source.errorString();
		if (target.write(bytes) != bytes.size())
			return target.errorString();
	}
	else
	{
		QImageWriter writer{ &target, QImageWriter::imageFormatsForMimeType(mimeType.toLatin1()).value(0) };
		if (isLossySaveAsType(mimeType))
			writer.setQuality(LossyEncodeQuality);
		if (!writer.write(_view->sourceImage()))
			return writer.errorString();
	}

	return target.commit() ? QString{} : target.errorString();
}

void ImageViewerWindow::removeCurrentImage(const std::function<bool(const QString& path)>& remove)
{
	if (!remove(currentImagePath()))
	{
		resumeReleasedAnimation(); // released for a file operation that then failed or was cancelled
		return;
	}

	int remaining = adjacentIndex(Direction::Next);
	if (remaining < 0)
		remaining = adjacentIndex(Direction::Previous);

	if (remaining < 0)
	{
		close();
		return;
	}

	// Erased, not skipped: a removal may leave the file on disk, and the caption counts only the images left.
	_imagePaths.removeAt(_index);
	_index = remaining > _index ? remaining - 1 : remaining; // in range even if showImage fails
	showImage(_index);
}
