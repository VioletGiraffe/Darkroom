#include "Windows/ImageViewerWindow.h"
#include "Core/Catalog.h"
#include "Core/Library.h"
#include "Utils.h"
#include "assert/advanced_assert.h"
#include "threading/cthreadpool.h"
#include "widgets/cimageviewerwidget.h"

DISABLE_COMPILER_WARNINGS
#include "resize/qimage_resize.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QKeySequence>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QSemaphore>
#include <QTimer>
#include <QUrl>
RESTORE_COMPILER_WARNINGS

#include <algorithm>
#include <functional>
#include <thread>
#include <utility>

namespace {

// Every viewer shares one pool, built on first use. Not QThreadPool::globalInstance(): thumbnail decoding runs
// there, and a scale that blocks the GUI thread must not queue behind it.
CThreadPool& scalingPool()
{
	static CThreadPool pool{ std::max(std::thread::hardware_concurrency(), 2u) - 1, "image-viewer" };
	return pool;
}

// ImageProcessing::resize needs every body() call finished before this returns, so the pooled chunks are waited
// on and one runs here. Only ever called from the GUI thread, never from a pool thread that could starve itself.
void runChunksInParallel(size_t count, const std::function<void(size_t)>& body)
{
	if (count == 0)
		return;

	QSemaphore finished;
	for (size_t chunk = 1; chunk < count; ++chunk)
		scalingPool().enqueue([&finished, &body, chunk] { body(chunk); finished.release(); });

	body(0);
	finished.acquire(static_cast<int>(count - 1));
}

}

void ImageViewerWindow::showForImages(Library* library, QStringList imagePaths, int startIndex, QWidget* dialogParent)
{
	assert_and_return_r(startIndex >= 0 && startIndex < imagePaths.size(), );

	if (const QString& startPath = imagePaths.at(startIndex); !QFileInfo::exists(startPath))
	{
		reportMissingFile(dialogParent, startPath);
		return;
	}

	auto* window = new ImageViewerWindow(library, std::move(imagePaths), startIndex);
	window->setAttribute(Qt::WA_DeleteOnClose);
	window->showFullScreen();
}

ImageViewerWindow::ImageViewerWindow(Library* library, QStringList imagePaths, int startIndex)
	: _library(library)
	, _imagePaths(std::move(imagePaths))
	, _index(startIndex)
{
	_view = new CImageViewerWidget(this);
	setCentralWidget(_view);
	_view->installEventFilter(this);

	const ImageProcessing::ParallelForFn parallelFor = runChunksInParallel;
	_view->setImageScaler([parallelFor](QImage& dest, const QImage& source, const QRect& srcRect) {
		if (!ImageProcessing::resize(dest, source, srcRect, parallelFor))
			CImageViewerWidget::smoothScale(dest, source, srcRect);
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
	QAction* copyAction = editMenu->addAction(tr("Copy image"), this, [this] { _view->copyToClipboard(); });
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

	QMenu* imageMenu = new QMenu(tr("Image"), menuBar);
	_previousAction = imageMenu->addAction(tr("Previous"), this, [this] { showImage(adjacentIndex(Direction::Previous)); });
	_previousAction->setShortcuts({ QKeySequence(Qt::Key_Left), QKeySequence(Qt::Key_PageUp) });
	_nextAction = imageMenu->addAction(tr("Next"), this, [this] { showImage(adjacentIndex(Direction::Next)); });
	_nextAction->setShortcuts({ QKeySequence(Qt::Key_Right), QKeySequence(Qt::Key_PageDown) });

	if (_library)
	{
		imageMenu->addSeparator();
		_bestAction = imageMenu->addAction(tr("Best"), this, &ImageViewerWindow::toggleBest);
		_bestAction->setCheckable(true);
		_bestAction->setShortcut(QKeySequence(Qt::Key_B));
	}

	menuBar->addMenu(fileMenu);
	menuBar->addMenu(editMenu);
	menuBar->addMenu(viewMenu);
	menuBar->addMenu(imageMenu);

	// A shortcut only matches while one of its action's widgets is visible, and the menu bar is hidden in
	// fullscreen, so the window holds every action as well.
	for (const QMenu* menu : { fileMenu, editMenu, viewMenu, imageMenu })
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
	updateLibraryActions();
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

MediaId ImageViewerWindow::currentMediaId() const
{
	if (!_library)
		return {};

	// The file is on disk - it was just displayed - so its identity derives correctly here.
	const MediaId id = MediaId::fromFile(currentPath());
	return _library->catalog().containsMediaItem(id) ? id : MediaId{};
}

void ImageViewerWindow::toggleBest()
{
	const MediaId id = currentMediaId();
	if (!id.isValid())
		return;

	Catalog& catalog = _library->catalog();
	if (catalog.mediaItemHasLabel(id, Catalog::BestLabelId))
		catalog.removeLabel(id, Catalog::BestLabelId);
	else
		catalog.addLabel(id, Catalog::BestLabelId);

	updateLibraryActions();
}

void ImageViewerWindow::updateLibraryActions()
{
	if (!_bestAction)
		return;

	const MediaId id = currentMediaId();
	_bestAction->setEnabled(id.isValid());
	_bestAction->setChecked(id.isValid() && _library->catalog().mediaItemHasLabel(id, Catalog::BestLabelId));
}
