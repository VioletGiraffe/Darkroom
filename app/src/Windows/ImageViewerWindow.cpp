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
#include <QSemaphore>
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

void ImageViewerWindow::showForImages(Library* library, QStringList imagePaths, int startIndex, QWidget* parent)
{
	assert_and_return_r(startIndex >= 0 && startIndex < imagePaths.size(), );

	if (const QString& startPath = imagePaths.at(startIndex); !QFileInfo::exists(startPath))
	{
		reportMissingFile(parent, startPath);
		return;
	}

	auto* window = new ImageViewerWindow(library, std::move(imagePaths), startIndex, parent);
	window->setAttribute(Qt::WA_DeleteOnClose);
	window->show();
}

ImageViewerWindow::ImageViewerWindow(Library* library, QStringList imagePaths, int startIndex, QWidget* parent)
	: QMainWindow(parent)
	, _library(library)
	, _imagePaths(std::move(imagePaths))
	, _index(startIndex)
{
	_view = new CImageViewerWidget(this);
	setCentralWidget(_view);

	const ImageProcessing::ParallelForFn parallelFor = runChunksInParallel;
	_view->setImageScaler([parallelFor](QImage& dest, const QImage& source, const QRect& srcRect) {
		if (!ImageProcessing::resize(dest, source, srcRect, parallelFor))
			CImageViewerWidget::smoothScale(dest, source, srcRect);
	});

	buildMenus();
	showImage(_index);

	if (!restoreWindowGeometry(this, "imageViewerWindow"))
	{
		resize(1200, 800);
		setWindowState(Qt::WindowMaximized);
	}
}

ImageViewerWindow::~ImageViewerWindow()
{
	saveWindowGeometry(this, "imageViewerWindow");
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
	_previousAction->setEnabled(adjacentIndex(Direction::Previous) >= 0);
	_nextAction->setEnabled(adjacentIndex(Direction::Next) >= 0);
	updateLibraryActions();
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
