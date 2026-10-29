#include "Windows/FrameViewerWindow.h"
#include "Windows/ImageViewerWindow.h"
#include "UiComponents/ImageFilesSource.h"
#include "UiComponents/MediaGrid.h"
#include "UiComponents/ThumbnailWidget.h"
#include "Utils.h"
#include "compiler/compiler_warnings_control.h"
#include "utils/filemanager.h"

DISABLE_COMPILER_WARNINGS
#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFileInfo>
#include <QListWidgetItem>
#include <QMenu>
#include <QSettings>
#include <QShortcut>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
RESTORE_COMPILER_WARNINGS

static constexpr int DEFAULT_THUMBNAIL_SIZE = 200;
static constexpr int MIN_THUMBNAIL_SIZE = 60;
static constexpr int MAX_THUMBNAIL_SIZE = 600;
static const QString SETTINGS_KEY_THUMBNAIL_SIZE = "frameViewer/thumbnailSize";

FrameViewerWindow::FrameViewerWindow(QWidget* parent)
	: QWidget{ parent, Qt::Window }
{
	setWindowTitle(tr("Frame Viewer"));
	resize(1200, 800);

	_thumbnailSize = qBound(MIN_THUMBNAIL_SIZE, QSettings{}.value(SETTINGS_KEY_THUMBNAIL_SIZE, DEFAULT_THUMBNAIL_SIZE).toInt(), MAX_THUMBNAIL_SIZE);

	auto* layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);

	_grid = new MediaGrid(this);
	_grid->setObjectName("tileGrid");   // the central sheet leaves this grid's cells unpainted: framed thumbnails show their own selection
	_grid->setFrameShape(QFrame::NoFrame);
	_grid->setUniformItemSizes(true);
	_grid->setDragEnabled(true);
	_grid->setDragUrlsProvider([this](const QList<QListWidgetItem*>& items) {
		QList<QUrl> urls;
		for (const QListWidgetItem* item : items)
			urls.push_back(QUrl::fromLocalFile(_imagePaths.at(_grid->row(item))));
		return urls;
	});
	_grid->setCardFactory([this](QListWidgetItem* item) { return buildThumbnail(item); });
	_grid->setZoomHandler([this](int steps) { zoomThumbnails(steps); });
	_grid->setContextMenuPolicy(Qt::CustomContextMenu);
	connect(_grid, &QWidget::customContextMenuRequested, this, [this](QPoint pos) {
		const QPoint globalPos = _grid->viewport()->mapToGlobal(pos);
		if (QListWidgetItem* item = _grid->itemAt(pos))
			showThumbnailContextMenu(*item, globalPos);
		else
			showBackgroundContextMenu(globalPos);
	});
	connect(_grid, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) { openInImageViewer(*item); });
	// Enter opens on macOS too: item views there open with Cmd+O and leave Enter to editing
	const auto openCurrentFrame = [this] {
		if (const QListWidgetItem* current = _grid->currentItem())
			openInImageViewer(*current);
	};
	new QShortcut(QKeySequence(Qt::Key_Return), _grid, openCurrentFrame, Qt::WidgetShortcut);
	new QShortcut(QKeySequence(Qt::Key_Enter), _grid, openCurrentFrame, Qt::WidgetShortcut);
	layout->addWidget(_grid);

	// Coalesce wheel steps into one rebuild.
	_zoomDebounce = new QTimer(this);
	_zoomDebounce->setSingleShot(true);
	_zoomDebounce->setInterval(100);
	connect(_zoomDebounce, &QTimer::timeout, this, &FrameViewerWindow::applyThumbnailSize);

	auto* escShortcut = new QShortcut(Qt::Key_Escape, this);
	connect(escShortcut, &QShortcut::activated, this, &QWidget::close);
}

void FrameViewerWindow::showForFolder(const QString& folderPath, const QString& title)
{
	_folderPath = folderPath;
	setWindowTitle(title.isEmpty() ? tr("Frame Viewer") : title);
	refreshDisplay();
	if (!folderPath.isEmpty())
	{
		show();
		raise();
		activateWindow();
	}
}

void FrameViewerWindow::refreshDisplay()
{
	_grid->clear();
	_imagePaths.clear();

	if (_folderPath.isEmpty())
	{
		_grid->setEmptyMessage(tr("Select a video to view its frames"));
		return;
	}

	QDir dir(_folderPath);
	if (!dir.exists())
	{
		_grid->setEmptyMessage(tr("Folder not found: %1").arg(_folderPath));
		return;
	}

	_grid->setEmptyMessage(tr("No images found in this folder"));
	const QStringList imageFiles = listFrameImageFiles(dir);
	_imagePaths.reserve(imageFiles.size());
	const QSize cell{ _thumbnailSize, _thumbnailSize };
	for (const QString& fileName : std::as_const(imageFiles))
	{
		_imagePaths << _folderPath + "/" + fileName;
		(new QListWidgetItem(_grid))->setSizeHint(cell);
	}

	_grid->scrollToTop();
	_grid->ensureVisibleCardsExist();
}

QWidget* FrameViewerWindow::buildThumbnail(QListWidgetItem* item)
{
	const QString& filePath = _imagePaths.at(_grid->row(item));
	// No drag or activation of its own: the grid drags the selection and activates the row.
	auto* thumbnail = new ThumbnailWidget(ImageFilesSource{ QStringList{ filePath } }, QFileInfo{ filePath }.completeBaseName(), {}, nullptr);
	thumbnail->setFixedSize(_thumbnailSize, _thumbnailSize);
	return thumbnail;
}

void FrameViewerWindow::openInImageViewer(const QListWidgetItem& item)
{
	ImageViewerWindow::showForImages(_imagePaths, _grid->row(&item), this);
}

void FrameViewerWindow::zoomThumbnails(int steps)
{
	const int next = MediaGrid::zoomedSize(_thumbnailSize, steps, MIN_THUMBNAIL_SIZE, MAX_THUMBNAIL_SIZE);
	if (next == _thumbnailSize)
		return;

	_thumbnailSize = next;
	QSettings{}.setValue(SETTINGS_KEY_THUMBNAIL_SIZE, _thumbnailSize);
	_zoomDebounce->start();
}

void FrameViewerWindow::applyThumbnailSize()
{
	const QSize cell{ _thumbnailSize, _thumbnailSize };
	for (int row = 0, rows = _grid->count(); row < rows; ++row)
		_grid->item(row)->setSizeHint(cell);
	_grid->discardAllCards();
	_grid->ensureVisibleCardsExist();
}

void FrameViewerWindow::showThumbnailContextMenu(QListWidgetItem& item, QPoint globalPos)
{
	const QString path = _imagePaths.at(_grid->row(&item));
	QStringList targetPaths;
	for (const QListWidgetItem* target : MediaGrid::effectiveSelection(item))
		targetPaths.push_back(QDir::toNativeSeparators(_imagePaths.at(_grid->row(target))));

	QMenu menu;
	menu.addAction(revealInFileManagerActionText(), [path, this] {
		if (!revealInFileManager(path))
			reportMissingFile(this, path);
	});
	menu.addAction(targetPaths.size() > 1 ? tr("Copy file paths") : tr("Copy file path"), [targetPaths] {
		QApplication::clipboard()->setText(targetPaths.join('\n'));
	});
	menu.exec(globalPos);

	// Null for a row far from the viewport: the menu key opens the menu on the current row wherever it is
	if (QWidget* card = _grid->itemWidget(&item))
		clearStuckHoverIfCursorLeft(card);
}

void FrameViewerWindow::showBackgroundContextMenu(QPoint globalPos)
{
	if (!QFileInfo(_folderPath).isDir())
		return;

	QMenu menu;
	menu.addAction(revealInFileManagerActionText(), [this] {
		if (!revealInFileManager(_folderPath))
			reportMissingFile(this, _folderPath);
	});
	menu.exec(globalPos);
}
