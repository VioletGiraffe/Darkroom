#include "Windows/ImageViewerWindow.h"
#include "Core/CpuThreadPool.h"
#include "Shortcuts.h"
#include "Utils.h"
#include "assert/advanced_assert.h"
#include "widgets/cimageviewerwidget.h"

DISABLE_COMPILER_WARNINGS
#include "resize/qimage_resize.h"

#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QKeySequence>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QTimer>
#include <QUrl>
RESTORE_COMPILER_WARNINGS

#include <functional>
#include <utility>

ImageViewerWindow* ImageViewerWindow::showForImages(QStringList imagePaths, int startIndex, QWidget* parent,
	std::function<void(int index)> onImageChanged)
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

void ImageViewerWindow::addImageAction(QAction* action, std::function<void(int index)> updateForImage)
{
	_imageMenu->addAction(action);
	addAction(action); // for the shortcut, see buildMenus
	updateForImage(_index);
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
		if (isAffected(viewer->currentPath()))
			viewer->_view->closeFile();
	}
}

ImageViewerWindow::ImageViewerWindow(QStringList imagePaths, int startIndex, QWidget* parent)
	: QMainWindow(parent)
	, _imagePaths(std::move(imagePaths))
	, _index(startIndex)
{
	_instances.push_back(this);

	_view = new CImageViewerWidget(this);
	setCentralWidget(_view);
	_view->installEventFilter(this);

	const ImageProcessing::ParallelForFn parallelFor = [](size_t count, const std::function<void(size_t)>& body) {
		cpuThreadPool().parallelFor(count, body);
	};
	_view->setImageScaler([parallelFor](QImage& dest, const QImage& source, const QRect& srcRect) {
		if (!ImageProcessing::resize(dest, source, srcRect, parallelFor))
			CImageViewerWidget::smoothScaleQt(dest, source, srcRect);
	});

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

	QMenu* fileMenu = new QMenu(tr("File"), menuBar);
	fileMenu->addAction(revealInFileManagerActionText(), this, [this] {
		if (!revealInFileManager(currentPath()))
			reportMissingFile(this, currentPath());
	});
	fileMenu->addAction(tr("Open in system app"), this, [this] {
		QDesktopServices::openUrl(QUrl::fromLocalFile(currentPath()));
	});
	fileMenu->addSeparator();
	QAction* closeAction = fileMenu->addAction(tr("Close"), this, &QWidget::close);
	closeAction->setShortcuts({ QKeySequence(Qt::Key_Escape), QKeySequence::Close });

	QMenu* editMenu = new QMenu(tr("Edit"), menuBar);
	QAction* copyAction = editMenu->addAction(tr("Copy image"), this, &ImageViewerWindow::copyImageAndFileToClipboard);
	copyAction->setShortcut(QKeySequence::Copy);

	QMenu* viewMenu = new QMenu(tr("View"), menuBar);
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

	_imageMenu = new QMenu(tr("Image"), menuBar);
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

	_index = index;
	setWindowTitle(QFileInfo{ path }.completeBaseName());
	_windowIconTimer->start();
	_previousAction->setEnabled(adjacentIndex(Direction::Previous) >= 0);
	_nextAction->setEnabled(adjacentIndex(Direction::Next) >= 0);
	for (const auto& updateForImage : _imageActionUpdaters)
		updateForImage(index);

	if (_onImageChanged)
		_onImageChanged(index);
}

bool ImageViewerWindow::eventFilter(QObject* watched, QEvent* event)
{
	if (event->type() == QEvent::MouseButtonDblClick && static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton)
	{
		toggleFullScreen();
		// Swallowed: the widget's default double-click handling forwards to mousePressEvent and starts a pan.
		return true;
	}

	return QMainWindow::eventFilter(watched, event);
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
	mime->setUrls({ QUrl::fromLocalFile(currentPath()) });
	QGuiApplication::clipboard()->setMimeData(mime);
}

void ImageViewerWindow::removeCurrentImage(const std::function<bool(int index)>& remove)
{
	const bool wasAnimated = _view->isAnimated();
	if (!remove(_index))
	{
		// Released for a file operation that then failed or was cancelled
		if (wasAnimated && !_view->isAnimated())
			_view->displayImage(currentPath(), false); // resumes the animation
		return;
	}

	// Cleared so that browsing skips it as it skips a deleted file: a removal may leave the file on disk.
	_imagePaths[_index].clear();

	int remaining = adjacentIndex(Direction::Next);
	if (remaining < 0)
		remaining = adjacentIndex(Direction::Previous);

	if (remaining >= 0)
		showImage(remaining);
	else
		close();
}
