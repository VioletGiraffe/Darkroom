#include "Windows/FrameViewerWindow.h"
#include "Windows/ImageViewerWindow.h"
#include "Theme/Style.h"
#include "Theme/Theme.h"
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
#include <QLabel>
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

	_instructionLabel = new QLabel(this);
	_instructionLabel->setAlignment(Qt::AlignCenter);
	Style::applyThemedSheet(_instructionLabel, [] {
		return QStringLiteral("font-size: 16pt; color: %1;").arg(Theme::current().instructionText.name());
	});
	_instructionLabel->hide();
	layout->addWidget(_instructionLabel);

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
		showInstruction(tr("Select a video to view its frames"));
		return;
	}

	QDir dir(_folderPath);
	if (!dir.exists())
	{
		showInstruction(tr("Folder not found: %1").arg(_folderPath));
		return;
	}

	const QStringList imageFiles = listFrameImageFiles(dir);
	if (imageFiles.isEmpty())
	{
		showInstruction(tr("No images found in this folder"));
		return;
	}

	_instructionLabel->hide();
	_grid->show();

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

void FrameViewerWindow::showInstruction(const QString& text)
{
	_instructionLabel->setText(text);
	_instructionLabel->show();
	_grid->hide();
}

QWidget* FrameViewerWindow::buildThumbnail(QListWidgetItem* item)
{
	const int index = _grid->row(item);
	const QString& filePath = _imagePaths.at(index);
	// No drag of its own: the grid drags the whole selection.
	auto* thumbnail = new ThumbnailWidget(ImageFilesSource{ QStringList{ filePath } }, QFileInfo{ filePath }.completeBaseName(), {}, nullptr);
	thumbnail->setFixedSize(_thumbnailSize, _thumbnailSize);
	thumbnail->setContextMenuPolicy(Qt::CustomContextMenu);
	connect(thumbnail, &ThumbnailWidget::customContextMenuRequested, this, [this, thumbnail, filePath](const QPoint& pos) {
		showThumbnailContextMenu(*thumbnail, filePath, pos);
	});
	thumbnail->setOnActivatedCallback([this, index] {
		ImageViewerWindow::showForImages(_imagePaths, index, this);
	});
	return thumbnail;
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

void FrameViewerWindow::showThumbnailContextMenu(ThumbnailWidget& thumbnail, const QString& path, const QPoint& pos)
{
	QMenu menu;
	menu.addAction(revealInFileManagerActionText(), [path, this] {
		if (!revealInFileManager(path))
			reportMissingFile(this, path);
	});
	menu.addAction(tr("Copy file path"), [path] {
		QApplication::clipboard()->setText(QDir::toNativeSeparators(path));
	});
	menu.exec(thumbnail.mapToGlobal(pos));

	clearStuckHoverIfCursorLeft(&thumbnail);
}
