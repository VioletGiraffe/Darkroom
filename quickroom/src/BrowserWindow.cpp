#include "BrowserWindow.h"
#include "IconTileWidget.h"
#include "UiComponents/MediaGrid.h"
#include "UiComponents/ThumbnailWidget.h"
#include "Windows/ImageViewerWindow.h"
#include "Windows/VideoPlayerWindow.h"
#include "Utils.h"
#include "assert/advanced_assert.h"
#include "compiler/compiler_warnings_control.h"
#include "utils/naturalsorting/cnaturalsorterqcollator.h"

DISABLE_COMPILER_WARNINGS
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QKeySequence>
#include <QLineEdit>
#include <QListWidgetItem>
#include <QMenu>
#include <QMetaObject>
#include <QMouseEvent>
#include <QScrollBar>
#include <QSettings>
#include <QShortcut>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyle>
#include <QTimer>
#include <QToolBar>
#include <QUrl>
RESTORE_COMPILER_WARNINGS

namespace {

const QString LAST_FOLDER_KEY = QStringLiteral("browser/lastFolder");
const QString GEOMETRY_KEY    = QStringLiteral("browser/geometry");
const QString TILE_SIZE_KEY   = QStringLiteral("browser/tileSize");
constexpr int DEFAULT_TILE_SIZE = 160;
constexpr int MIN_TILE_SIZE  = 80;
constexpr int MAX_TILE_SIZE  = 400;
constexpr int TILE_SIZE_STEP = 20;

int tileSize()
{
	return qBound(MIN_TILE_SIZE, QSettings{}.value(TILE_SIZE_KEY, DEFAULT_TILE_SIZE).toInt(), MAX_TILE_SIZE);
}

enum class EntryKind { Folder, Image, Video };

class GridEntry final : public QListWidgetItem {
public:
	QString path;
	QString name;
	EntryKind kind = EntryKind::Folder;

	// Folders first, then natural name order.
	bool operator<(const QListWidgetItem& other) const override
	{
		const auto& b = static_cast<const GridEntry&>(other);
		if ((kind == EntryKind::Folder) != (b.kind == EntryKind::Folder))
			return kind == EntryKind::Folder;
		return NaturalSort::lessCaseInsensitive(name, b.name);
	}
};

} // anonymous namespace

BrowserWindow::BrowserWindow()
{
	setupUi();

	if (!restoreWindowGeometry(this, GEOMETRY_KEY))
		resize(1200, 800);

	QString startPath = QSettings{}.value(LAST_FOLDER_KEY).toString();
	if (startPath.isEmpty() || !QFileInfo(startPath).isDir())
		startPath = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
	if (startPath.isEmpty() || !QFileInfo(startPath).isDir())
		startPath = QDir::homePath();
	navigateTo(startPath);
}

void BrowserWindow::setupUi()
{
	QToolBar* toolbar = addToolBar(tr("Navigation"));
	toolbar->setMovable(false);

	_backAction = toolbar->addAction(style()->standardIcon(QStyle::SP_ArrowBack), tr("Back"), this, &BrowserWindow::goBack);
	_backAction->setShortcuts(QKeySequence::Back);
	_forwardAction = toolbar->addAction(style()->standardIcon(QStyle::SP_ArrowForward), tr("Forward"), this, &BrowserWindow::goForward);
	_forwardAction->setShortcuts(QKeySequence::Forward);
	_upAction = toolbar->addAction(style()->standardIcon(QStyle::SP_ArrowUp), tr("Up"), this, &BrowserWindow::goUp);
	_upAction->setShortcut(QKeySequence(Qt::Key_Backspace));

	_pathEdit = new QLineEdit();
	connect(_pathEdit, &QLineEdit::returnPressed, this, [this] {
		navigateTo(QDir::fromNativeSeparators(_pathEdit->text().trimmed()));
	});
	toolbar->addWidget(_pathEdit);

	auto* refreshShortcut = new QShortcut(QKeySequence::Refresh, this);
	connect(refreshShortcut, &QShortcut::activated, this, &BrowserWindow::refresh);

	_grid = new MediaGrid();
	_grid->setUniformItemSizes(true);
	_grid->setDragEnabled(true);
	_grid->setEmptyMessage(tr("No folders or supported media files here."));
	_grid->setCardFactory([this](QListWidgetItem* item) { return buildTile(item); });
	_grid->setDragUrlsProvider([](const QList<QListWidgetItem*>& items) {
		QList<QUrl> urls;
		for (const QListWidgetItem* item : items)
			urls.push_back(QUrl::fromLocalFile(static_cast<const GridEntry*>(item)->path));
		return urls;
	});
	_grid->viewport()->installEventFilter(this);
	setCentralWidget(_grid);

	const auto activateCurrent = [this] { activateEntry(_grid->currentItem()); };
	auto* returnShortcut = new QShortcut(QKeySequence(Qt::Key_Return), _grid, activateCurrent);
	returnShortcut->setContext(Qt::WidgetShortcut);
	auto* enterShortcut = new QShortcut(QKeySequence(Qt::Key_Enter), _grid, activateCurrent);
	enterShortcut->setContext(Qt::WidgetShortcut);

	// Coalesce wheel steps into one grid rebuild.
	_zoomDebounce = new QTimer(this);
	_zoomDebounce->setSingleShot(true);
	_zoomDebounce->setInterval(80);
	connect(_zoomDebounce, &QTimer::timeout, this, &BrowserWindow::applyTileSize);
}

bool BrowserWindow::listDirectory(const QString& path)
{
	const QDir dir(path);
	if (!dir.exists())
		return false;

	_grid->clear();
	_currentPath = dir.absolutePath();

	int folders = 0, images = 0, videos = 0;
	const int tile = tileSize();
	for (const QFileInfo& info : dir.entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot, QDir::Unsorted))
	{
		EntryKind kind;
		if (info.isDir())
			{ kind = EntryKind::Folder; ++folders; }
		else if (isSupportedImageFile(info.filePath()))
			{ kind = EntryKind::Image; ++images; }
		else if (isSupportedVideoFile(info.filePath()))
			{ kind = EntryKind::Video; ++videos; }
		else
			continue;

		auto* item = new GridEntry();
		item->path = info.absoluteFilePath();
		item->name = info.fileName();
		item->kind = kind;
		item->setSizeHint(QSize(tile, tile));
		_grid->addItem(item);
	}
	_grid->sortItems(Qt::AscendingOrder);
	_grid->scrollToTop();
	_grid->ensureVisibleCardsExist();

	_pathEdit->setText(QDir::toNativeSeparators(_currentPath));
	setWindowTitle((dir.dirName().isEmpty() ? QDir::toNativeSeparators(_currentPath) : dir.dirName()) + QStringLiteral(" - Quickroom"));
	statusBar()->showMessage(tr("%1 folders, %2 images, %3 videos").arg(folders).arg(images).arg(videos));
	QSettings{}.setValue(LAST_FOLDER_KEY, _currentPath);
	return true;
}

void BrowserWindow::navigateTo(const QString& path)
{
	const bool samePath = _historyIndex >= 0 && _history[static_cast<size_t>(_historyIndex)] == QDir(path).absolutePath();
	if (!listDirectory(path))
	{
		statusBar()->showMessage(tr("Cannot open \"%1\"").arg(QDir::toNativeSeparators(path)));
		_pathEdit->setText(QDir::toNativeSeparators(_currentPath));
		return;
	}
	if (!samePath)
	{
		// A new navigation discards the forward tail.
		_history.resize(static_cast<size_t>(_historyIndex + 1));
		_history.push_back(_currentPath);
		++_historyIndex;
	}
	updateNavigationActions();
}

void BrowserWindow::goBack()
{
	if (_historyIndex > 0 && listDirectory(_history[static_cast<size_t>(_historyIndex - 1)]))
		--_historyIndex;
	updateNavigationActions();
}

void BrowserWindow::goForward()
{
	if (_historyIndex + 1 < static_cast<int>(_history.size()) && listDirectory(_history[static_cast<size_t>(_historyIndex + 1)]))
		++_historyIndex;
	updateNavigationActions();
}

void BrowserWindow::goUp()
{
	QDir dir(_currentPath);
	if (dir.cdUp())
		navigateTo(dir.absolutePath());
}

void BrowserWindow::refresh()
{
	const int scrollPosition = _grid->verticalScrollBar()->value();
	if (listDirectory(_currentPath))
		_grid->verticalScrollBar()->setValue(scrollPosition);
}

void BrowserWindow::updateNavigationActions()
{
	_backAction->setEnabled(_historyIndex > 0);
	_forwardAction->setEnabled(_historyIndex + 1 < static_cast<int>(_history.size()));
	_upAction->setEnabled(!QDir(_currentPath).isRoot());
}

void BrowserWindow::activateEntry(const QListWidgetItem* item)
{
	if (!item)
		return;

	const auto& entry = static_cast<const GridEntry&>(*item);
	switch (entry.kind)
	{
	case EntryKind::Folder: navigateTo(entry.path); break;
	case EntryKind::Image:  viewImage(entry.path);  break;
	case EntryKind::Video:  playVideo(entry.path);  break;
	}
}

void BrowserWindow::viewImage(const QString& path)
{
	QStringList imagePaths;
	int startIndex = -1;
	for (int row = 0, rows = _grid->count(); row < rows; ++row)
	{
		const auto& entry = static_cast<const GridEntry&>(*_grid->item(row));
		if (entry.kind != EntryKind::Image)
			continue;
		if (entry.path == path)
			startIndex = static_cast<int>(imagePaths.size());
		imagePaths.push_back(entry.path);
	}
	assert_and_return_r(startIndex >= 0, );

	ImageViewerWindow::showForImages(nullptr, imagePaths, startIndex, this,
		[this, imagePaths](int index) { selectAndScrollToPath(imagePaths[index]); });
}

void BrowserWindow::playVideo(const QString& path)
{
	if (!QFile::exists(path))
	{
		reportMissingFile(this, path);
		return;
	}
	VideoPlayerWindow::createPlayerWindow(nullptr, path, nullptr);
}

void BrowserWindow::showEntryContextMenu(const QString& path, QPoint globalPos)
{
	QMenu menu(this);
	menu.addAction(revealInFileManagerActionText(), this, [this, path] {
		if (!revealInFileManager(path))
			reportMissingFile(this, path);
	});
	menu.exec(globalPos);
}

void BrowserWindow::selectAndScrollToPath(const QString& path)
{
	for (int row = 0, rows = _grid->count(); row < rows; ++row)
	{
		QListWidgetItem* item = _grid->item(row);
		if (static_cast<const GridEntry*>(item)->path != path)
			continue;
		_grid->setCurrentItem(item);
		_grid->scrollToItem(item);
		return;
	}
}

QWidget* BrowserWindow::buildTile(QListWidgetItem* item)
{
	const auto& entry = static_cast<const GridEntry&>(*item);
	const auto onWheel = [this](int steps) { zoomTiles(steps); };
	const auto onContextMenu = [this, path = entry.path](QWidget* tile, QPoint pos) { showEntryContextMenu(path, tile->mapToGlobal(pos)); };

	if (entry.kind == EntryKind::Image)
	{
		auto* thumb = new ThumbnailWidget(entry.path, entry.name, tileSize(), nullptr);
		thumb->setOnActivatedCallback([this, path = entry.path] { viewImage(path); });
		thumb->setOnMouseWheelCallback(onWheel);
		connect(thumb, &QWidget::customContextMenuRequested, this, [thumb, onContextMenu](QPoint pos) { onContextMenu(thumb, pos); });
		return thumb;
	}

	auto* tile = new IconTileWidget(_iconProvider.icon(QFileInfo(entry.path)), entry.name, tileSize(), nullptr);
	if (entry.kind == EntryKind::Folder)
	{
		// Queued: navigating rebuilds the grid, which would delete this tile mid-handler.
		tile->setOnActivatedCallback([this, path = entry.path] {
			QMetaObject::invokeMethod(this, [this, path] { navigateTo(path); }, Qt::QueuedConnection);
		});
	}
	else
		tile->setOnActivatedCallback([this, path = entry.path] { playVideo(path); });
	tile->setOnMouseWheelCallback(onWheel);
	connect(tile, &QWidget::customContextMenuRequested, this, [tile, onContextMenu](QPoint pos) { onContextMenu(tile, pos); });
	return tile;
}

void BrowserWindow::zoomTiles(int steps)
{
	const int current = tileSize();
	const int next = qBound(MIN_TILE_SIZE, current + steps * TILE_SIZE_STEP, MAX_TILE_SIZE);
	if (next == current)
		return;

	QSettings{}.setValue(TILE_SIZE_KEY, next);
	_zoomDebounce->start();
}

void BrowserWindow::applyTileSize()
{
	const int tile = tileSize();
	for (int row = 0, rows = _grid->count(); row < rows; ++row)
		_grid->item(row)->setSizeHint(QSize(tile, tile));
	_grid->discardAllCards();
	_grid->ensureVisibleCardsExist();
}

bool BrowserWindow::eventFilter(QObject* watched, QEvent* event)
{
	if (event->type() == QEvent::MouseButtonPress)
	{
		const auto* mouseEvent = static_cast<const QMouseEvent*>(event);
		if (mouseEvent->button() == Qt::BackButton)
		{
			goBack();
			return true;
		}
		if (mouseEvent->button() == Qt::ForwardButton)
		{
			goForward();
			return true;
		}
	}
	return QMainWindow::eventFilter(watched, event);
}

void BrowserWindow::closeEvent(QCloseEvent* event)
{
	saveWindowGeometry(this, GEOMETRY_KEY);
	QMainWindow::closeEvent(event);
}
