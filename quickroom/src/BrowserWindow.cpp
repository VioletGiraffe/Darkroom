#include "BrowserWindow.h"
#include "FileOperations.h"
#include "IconTileWidget.h"
#include "UiComponents/MediaGrid.h"
#include "UiComponents/ThumbnailWidget.h"
#include "Windows/ImageViewerWindow.h"
#include "Windows/OpenFileRelease.h"
#include "Windows/VideoPlayerWindow.h"
#include "Utils.h"
#include "assert/advanced_assert.h"
#include "compiler/compiler_warnings_control.h"
#include "utils/naturalsorting/cnaturalsorterqcollator.h"
#include "widgets/cpersistenceenabler.h"

DISABLE_COMPILER_WARNINGS
#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QGraphicsOpacityEffect>
#include <QGuiApplication>
#include <QKeySequence>
#include <QLineEdit>
#include <QListWidgetItem>
#include <QMenu>
#include <QMetaObject>
#include <QMimeData>
#include <QMouseEvent>
#include <QPointer>
#include <QSettings>
#include <QShortcut>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStorageInfo>
#include <QStyle>
#include <QTimer>
#include <QToolBar>
#include <QUrl>
#include <QVBoxLayout>
RESTORE_COMPILER_WARNINGS

#include <algorithm>
#include <memory>
#include <ranges>
#include <unordered_map>
#include <vector>

namespace {

const QString LAST_FOLDER_KEY = QStringLiteral("browser/lastFolder");
const QString TILE_SIZE_KEY   = QStringLiteral("browser/tileSize");
constexpr int DEFAULT_TILE_SIZE = 160;
constexpr int MIN_TILE_SIZE  = 80;
constexpr int MAX_TILE_SIZE  = 400;
constexpr int TILE_SIZE_STEP = 20;
// Grid cells exceed the tile by this margin per side, leaving the view's selection background visible around it.
constexpr int SELECTION_RING = 6;

int savedTileSize()
{
	return qBound(MIN_TILE_SIZE, QSettings{}.value(TILE_SIZE_KEY, DEFAULT_TILE_SIZE).toInt(), MAX_TILE_SIZE);
}

QSize cellSize(int tile)
{
	const int side = tile + 2 * SELECTION_RING;
	return { side, side };
}

// A plain QWidget paints no background of its own, so the cell's selection background shows through the margin.
QWidget* inSelectionFrame(QWidget* tile)
{
	auto* frame = new QWidget();
	auto* layout = new QVBoxLayout(frame);
	layout->setContentsMargins(SELECTION_RING, SELECTION_RING, SELECTION_RING, SELECTION_RING);
	layout->addWidget(tile);
	return frame;
}

// A cut file's tile is drawn faded, as file managers draw theirs.
void setCutMark(QWidget* card, bool cut)
{
	if ((card->graphicsEffect() != nullptr) == cut)
		return;

	QGraphicsOpacityEffect* effect = nullptr;
	if (cut)
	{
		effect = new QGraphicsOpacityEffect();
		effect->setOpacity(0.5);
	}
	card->setGraphicsEffect(effect); // deletes the previous effect
}

[[nodiscard]] bool clipboardHasFiles()
{
	const QMimeData* mime = QGuiApplication::clipboard()->mimeData();
	return mime && !FileOperations::localFilePaths(*mime).empty();
}

// The requested folder, else the remembered one, else Pictures, else home. A requested folder that has gone
// missing falls back like an absent one.
QString resolveStartFolder(const QString& requested)
{
	const auto usable = [](const QString& path) { return !path.isEmpty() && QFileInfo(path).isDir(); };

	if (usable(requested))
		return requested;
	if (const QString remembered = QSettings{}.value(LAST_FOLDER_KEY).toString(); usable(remembered))
		return remembered;
	if (const QString pictures = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation); usable(pictures))
		return pictures;
	return QDir::homePath();
}

enum class EntryKind { Folder, Image, Video };

class GridEntry final : public QListWidgetItem {
public:
	QString path;
	QString name;
	EntryKind kind = EntryKind::Folder;
	// size and lastModified detect a file rewritten in place, whose tile is then stale.
	qint64 size = 0;
	QDateTime lastModified;

	// Folders first, then natural name order.
	bool operator<(const QListWidgetItem& other) const override
	{
		const auto& b = static_cast<const GridEntry&>(other);
		if ((kind == EntryKind::Folder) != (b.kind == EntryKind::Folder))
			return kind == EntryKind::Folder;
		return NaturalSort::lessCaseInsensitive(name, b.name);
	}
};

// The browser's entries for dir: folders and supported media files, unsorted.
std::vector<std::unique_ptr<GridEntry>> listedEntries(const QDir& dir)
{
	std::vector<std::unique_ptr<GridEntry>> entries;
	for (const QFileInfo& info : dir.entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot, QDir::Unsorted))
	{
		EntryKind kind;
		if (info.isDir())
			kind = EntryKind::Folder;
		else if (isSupportedImageFile(info.filePath()))
			kind = EntryKind::Image;
		else if (isSupportedVideoFile(info.filePath()))
			kind = EntryKind::Video;
		else
			continue;

		auto entry = std::make_unique<GridEntry>();
		entry->path = info.absoluteFilePath();
		entry->name = info.fileName();
		entry->kind = kind;
		entry->size = info.size();
		entry->lastModified = info.lastModified();
		entries.push_back(std::move(entry));
	}
	return entries;
}

// The row that keeps the sorted grid sorted with entry inserted.
int sortedInsertionRow(const QListWidget& grid, const GridEntry& entry)
{
	return *std::ranges::partition_point(std::views::iota(0, grid.count()), [&](int row) { return *grid.item(row) < entry; });
}

// path itself when it is a folder, else its closest ancestor that is; a filesystem root when none is.
QString nearestExistingFolder(QString path)
{
	while (!QFileInfo(path).isDir())
	{
		const QString parent = QFileInfo(path).absolutePath();
		if (parent == path)
			break;
		path = parent;
	}
	return path;
}

} // anonymous namespace

void BrowserWindow::showForFolder(const QString& folder, const QString& selectPath)
{
	auto* window = new BrowserWindow(resolveStartFolder(folder));
	window->setAttribute(Qt::WA_DeleteOnClose);
	window->show();
	// Scrolling to the item needs the laid-out viewport the window only has once shown.
	if (!selectPath.isEmpty())
		window->selectAndScrollToPath(selectPath);
}

BrowserWindow::BrowserWindow(const QString& folder)
	: _tileSize{ savedTileSize() }
{
	setupUi();

	resize(1200, 800);
	enablePersistence(this, "browser/window", CPersistenceEnabler::Delayed{ true }, CPersistenceEnabler::SetDefaultSize{ false });

	navigateTo(folder);
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
	// The target performs a move itself, possibly after the drag returns; released files let it, or its retry, succeed.
	_grid->setDragActions(Qt::CopyAction | Qt::MoveAction);
	_grid->setDragFinishedHandler([](Qt::DropAction action, const QList<QUrl>& urls) {
		if (action != Qt::MoveAction && action != Qt::TargetMoveAction)
			return;
		QStringList paths;
		for (const QUrl& url : urls)
			paths.push_back(url.toLocalFile());
		releaseMediaFilesHeldOpen(paths);
	});
	_grid->setContextMenuPolicy(Qt::CustomContextMenu);
	connect(_grid, &QWidget::customContextMenuRequested, this, [this](QPoint pos) {
		const QPoint globalPos = _grid->viewport()->mapToGlobal(pos);
		if (const auto* entry = static_cast<const GridEntry*>(_grid->itemAt(pos)))
			showEntryContextMenu(entry->path, entry->kind == EntryKind::Folder, globalPos);
		else
			showBackgroundContextMenu(globalPos);
	});
	_grid->viewport()->installEventFilter(this);
	setCentralWidget(_grid);
	setAcceptDrops(true);

	// Grid-only: the path field keeps its own keys.
	const auto activateCurrent = [this] { activateEntry(_grid->currentItem()); };
	new QShortcut(QKeySequence(Qt::Key_Return), _grid, activateCurrent, Qt::WidgetShortcut);
	new QShortcut(QKeySequence(Qt::Key_Enter), _grid, activateCurrent, Qt::WidgetShortcut);
	new QShortcut(QKeySequence(Qt::Key_Delete), _grid, [this] { deletePaths(selectedPaths(), PathDeletion::Mode::Trash); }, Qt::WidgetShortcut);
	new QShortcut(QKeySequence(Qt::SHIFT | Qt::Key_Delete), _grid, [this] { deletePaths(selectedPaths(), PathDeletion::Mode::Permanent); }, Qt::WidgetShortcut);
	new QShortcut(QKeySequence::Copy, _grid, [this] { FileOperations::putPathsOnClipboard(selectedPaths(), false); }, Qt::WidgetShortcut);
	// Not QKeySequence::Cut: on Windows and Linux it includes Shift+Del, the permanent deletion above.
	new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_X), _grid, [this] { FileOperations::putPathsOnClipboard(selectedPaths(), true); }, Qt::WidgetShortcut);
	new QShortcut(QKeySequence::Paste, _grid, [this] { pasteInto(_currentPath); }, Qt::WidgetShortcut);

	connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, this, &BrowserWindow::updateCutMarks);
	updateCutMarks();

	// Coalesce wheel steps into one grid rebuild.
	_zoomDebounce = new QTimer(this);
	_zoomDebounce->setSingleShot(true);
	_zoomDebounce->setInterval(80);
	connect(_zoomDebounce, &QTimer::timeout, this, &BrowserWindow::applyTileSize);

	// Not restarted by further changes: a long copy into the folder refreshes it periodically, not only once it ends.
	_refreshDebounce = new QTimer(this);
	_refreshDebounce->setSingleShot(true);
	_refreshDebounce->setInterval(300);
	connect(_refreshDebounce, &QTimer::timeout, this, &BrowserWindow::refresh);

	_folderWatcher = new QFileSystemWatcher(this);
	connect(_folderWatcher, &QFileSystemWatcher::directoryChanged, this, [this] {
		if (!_refreshDebounce->isActive())
			_refreshDebounce->start();
	});
}

bool BrowserWindow::listDirectory(const QString& path)
{
	const QDir dir(path);
	if (!dir.exists())
		return false;

	_grid->clear();
	_currentPath = dir.absolutePath();

	if (const QStringList watched = _folderWatcher->directories(); !watched.empty())
		_folderWatcher->removePaths(watched);
	_folderWatcher->addPath(_currentPath);

	const QSize cell = cellSize(_tileSize);
	for (std::unique_ptr<GridEntry>& entry : listedEntries(dir))
	{
		entry->setSizeHint(cell);
		_grid->addItem(entry.release());
	}
	_grid->sortItems(Qt::AscendingOrder);
	_grid->scrollToTop();
	_grid->ensureVisibleCardsExist();

	_pathEdit->setText(QDir::toNativeSeparators(_currentPath));
	setWindowTitle((dir.dirName().isEmpty() ? QDir::toNativeSeparators(_currentPath) : dir.dirName()) + QStringLiteral(" - Quickroom"));
	updateStatusCounts();
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
	const QDir dir(_currentPath);
	if (!dir.exists())
	{
		navigateTo(resolveStartFolder(nearestExistingFolder(_currentPath)));
		return;
	}

	std::unordered_map<QString, std::unique_ptr<GridEntry>> onDisk;
	for (std::unique_ptr<GridEntry>& entry : listedEntries(dir))
	{
		QString path = entry->path;
		onDisk.emplace(std::move(path), std::move(entry));
	}

	// Afterwards onDisk holds only the entries the grid lacks.
	bool changed = false;
	for (int row = _grid->count() - 1; row >= 0; --row)
	{
		auto* shown = static_cast<GridEntry*>(_grid->item(row));
		const auto found = onDisk.find(shown->path);
		if (found == onDisk.end() || found->second->kind != shown->kind)
		{
			delete _grid->takeItem(row);
			changed = true;
			continue;
		}

		const GridEntry& current = *found->second;
		if (shown->kind != EntryKind::Folder && (current.size != shown->size || current.lastModified != shown->lastModified))
		{
			shown->size = current.size;
			shown->lastModified = current.lastModified;
			_grid->removeItemWidget(shown);
			changed = true;
		}
		onDisk.erase(found);
	}

	const QSize cell = cellSize(_tileSize);
	for (auto& [path, entry] : onDisk)
	{
		entry->setSizeHint(cell);
		const int row = sortedInsertionRow(*_grid, *entry);
		_grid->insertItem(row, entry.release());
		changed = true;
	}

	if (!changed)
		return;
	_grid->ensureVisibleCardsExist();
	updateStatusCounts();
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

	// The viewer is parentless and can outlive this window.
	const QPointer<BrowserWindow> self{ this };
	ImageViewerWindow* const viewer = ImageViewerWindow::showForImages(imagePaths, startIndex, this,
		[self, imagePaths](int index) {
			if (self)
				self->selectAndScrollToPath(imagePaths[index]);
		});
	if (!viewer)
		return;

	FileOperations::installDeleteActions(*viewer, imagePaths, entryRemover());
}

void BrowserWindow::playVideo(const QString& path)
{
	if (!QFile::exists(path))
	{
		reportMissingFile(this, path);
		return;
	}
	FileOperations::installDeleteActions(*VideoPlayerWindow::createPlayerWindow(path, nullptr), entryRemover());
}

std::function<void(const QString& path)> BrowserWindow::entryRemover()
{
	return [self = QPointer<BrowserWindow>{ this }](const QString& path) {
		if (self)
			self->removeEntries({ path });
	};
}

void BrowserWindow::showEntryContextMenu(const QString& path, bool isFolder, QPoint globalPos)
{
	QMenu menu(this);
	menu.addAction(revealInFileManagerActionText(), this, [this, path] {
		if (!revealInFileManager(path))
			reportMissingFile(this, path);
	});

	// A tile outside the selection is acted on alone.
	QStringList targets = selectedPaths();
	if (!targets.contains(path))
		targets = { path };

	menu.addSeparator();
	menu.addAction(tr("Cut"), this, [targets] { FileOperations::putPathsOnClipboard(targets, true); });
	menu.addAction(tr("Copy"), this, [targets] { FileOperations::putPathsOnClipboard(targets, false); });
	if (isFolder)
		menu.addAction(tr("Paste into folder"), this, [this, path] { pasteInto(path); })->setEnabled(clipboardHasFiles());

	// Queued, like folder activation in buildTile: the tile showing this menu may be among those removed.
	const auto addDeleteAction = [this, &menu, targets](PathDeletion::Mode mode) {
		menu.addAction(FileOperations::deleteActionText(mode), this, [this, targets, mode] {
			QMetaObject::invokeMethod(this, [this, targets, mode] { deletePaths(targets, mode); }, Qt::QueuedConnection);
		});
	};
	menu.addSeparator();
	addDeleteAction(PathDeletion::Mode::Trash);
	addDeleteAction(PathDeletion::Mode::Permanent);

	menu.exec(globalPos);
}

void BrowserWindow::showBackgroundContextMenu(QPoint globalPos)
{
	QMenu menu(this);
	menu.addAction(tr("Paste"), this, [this] { pasteInto(_currentPath); })->setEnabled(clipboardHasFiles());
	menu.exec(globalPos);
}

void BrowserWindow::pasteInto(const QString& folder)
{
	const QMimeData* mime = QGuiApplication::clipboard()->mimeData();
	if (!mime)
		return;

	const QStringList sources = FileOperations::localFilePaths(*mime);
	if (sources.empty())
		return;

	const bool cut = FileOperations::isMarkedAsCut(*mime);
	if (cut)
		QGuiApplication::clipboard()->clear(); // invalidates mime
	FileOperations::startTransfer(cut ? TransferKind::Move : TransferKind::Copy, sources, folder, this);
}

void BrowserWindow::updateCutMarks()
{
	_cutPathKeys.clear();
	if (const QMimeData* mime = QGuiApplication::clipboard()->mimeData(); mime && FileOperations::isMarkedAsCut(*mime))
	{
		for (const QString& path : FileOperations::localFilePaths(*mime))
			_cutPathKeys.insert(pathComparisonKey(path));
	}

	for (int row = 0, rows = _grid->count(); row < rows; ++row)
	{
		QListWidgetItem* item = _grid->item(row);
		if (QWidget* card = _grid->itemWidget(item))
			setCutMark(card, isCut(static_cast<const GridEntry*>(item)->path));
	}
}

bool BrowserWindow::isCut(const QString& path) const
{
	return !_cutPathKeys.empty() && _cutPathKeys.contains(pathComparisonKey(path));
}

QString BrowserWindow::dropTargetFolder(const QDropEvent& event, const QStringList& sources) const
{
	const QPoint viewportPos = _grid->viewport()->mapFrom(this, event.position().toPoint());
	const auto* entry = _grid->viewport()->rect().contains(viewportPos) ? static_cast<const GridEntry*>(_grid->itemAt(viewportPos)) : nullptr;
	const bool ontoFolderTile = entry && entry->kind == EntryKind::Folder;
	if (!ontoFolderTile && event.source() == _grid)
		return {};

	const QString target = ontoFolderTile ? entry->path : _currentPath;
	const QString targetKey = pathComparisonKey(target);
	const bool intoASource = std::any_of(sources.cbegin(), sources.cend(), [&targetKey](const QString& source) { return pathComparisonKey(source) == targetKey; });
	return intoASource ? QString() : target;
}

Qt::DropAction BrowserWindow::dropAction(const QDropEvent& event) const
{
	const Qt::KeyboardModifiers modifiers = event.modifiers();
#ifdef Q_OS_MACOS
	// Finder's keys: Option copies, Command moves. Qt reports them as Alt and Control.
	const bool copyKey = modifiers.testFlag(Qt::AltModifier);
	const bool moveKey = modifiers.testFlag(Qt::ControlModifier);
#else
	const bool copyKey = modifiers.testFlag(Qt::ControlModifier);
	const bool moveKey = modifiers.testFlag(Qt::ShiftModifier);
#endif
	const bool move = !copyKey && (moveKey || _dropSourcesOnShownVolume);
	const Qt::DropAction preferred = move ? Qt::MoveAction : Qt::CopyAction;
	if (event.possibleActions().testFlag(preferred))
		return preferred;
	return event.possibleActions().testFlag(Qt::CopyAction) ? Qt::CopyAction : Qt::IgnoreAction;
}

void BrowserWindow::dragEnterEvent(QDragEnterEvent* event)
{
	const QStringList sources = FileOperations::localFilePaths(*event->mimeData());
	if (sources.empty())
		return;

	_dropSourcesOnShownVolume = QStorageInfo(sources.front()).rootPath() == QStorageInfo(_currentPath).rootPath();
	dragMoveEvent(event);
}

void BrowserWindow::dragMoveEvent(QDragMoveEvent* event)
{
	const Qt::DropAction action = dropAction(*event);
	if (action == Qt::IgnoreAction || dropTargetFolder(*event, FileOperations::localFilePaths(*event->mimeData())).isEmpty())
	{
		event->ignore();
		return;
	}
	event->setDropAction(action);
	event->accept();
}

void BrowserWindow::dropEvent(QDropEvent* event)
{
	const QStringList sources = FileOperations::localFilePaths(*event->mimeData());
	const QString target = dropTargetFolder(*event, sources);
	const Qt::DropAction action = dropAction(*event);
	if (sources.empty() || target.isEmpty() || action == Qt::IgnoreAction)
	{
		event->ignore();
		return;
	}

	// TargetMoveAction: the source must not delete the files, the move below does.
	event->setDropAction(action == Qt::MoveAction ? Qt::TargetMoveAction : action);
	event->accept();

	// Queued: the drag source waits for this handler, and a message box here would hold it.
	const TransferKind kind = action == Qt::MoveAction ? TransferKind::Move : TransferKind::Copy;
	QMetaObject::invokeMethod(this, [this, kind, sources, target] { FileOperations::startTransfer(kind, sources, target, this); }, Qt::QueuedConnection);
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

QStringList BrowserWindow::selectedPaths() const
{
	QStringList paths;
	for (const QListWidgetItem* item : _grid->selectedItems())
		paths.push_back(static_cast<const GridEntry*>(item)->path);
	return paths;
}

void BrowserWindow::deletePaths(const QStringList& paths, PathDeletion::Mode mode)
{
	removeEntries(FileOperations::deleteWithConfirmation(paths, mode, this));
}

void BrowserWindow::removeEntries(const QSet<QString>& paths)
{
	int rowAfterLastRemoved = -1;
	for (int row = _grid->count() - 1; row >= 0; --row)
	{
		if (!paths.contains(static_cast<const GridEntry*>(_grid->item(row))->path))
			continue;

		delete _grid->takeItem(row);
		// Bottom-up: the first removal sets the row, each one above it shifts that row up.
		rowAfterLastRemoved = rowAfterLastRemoved < 0 ? row : rowAfterLastRemoved - 1;
	}
	if (rowAfterLastRemoved < 0)
		return;

	if (QListWidgetItem* next = _grid->item(std::min(rowAfterLastRemoved, _grid->count() - 1)))
		_grid->setCurrentItem(next);
	_grid->ensureVisibleCardsExist();
	updateStatusCounts();
}

void BrowserWindow::updateStatusCounts()
{
	int folders = 0, images = 0, videos = 0;
	for (int row = 0, rows = _grid->count(); row < rows; ++row)
	{
		switch (static_cast<const GridEntry*>(_grid->item(row))->kind)
		{
		case EntryKind::Folder: ++folders; break;
		case EntryKind::Image:  ++images;  break;
		case EntryKind::Video:  ++videos;  break;
		}
	}
	statusBar()->showMessage(tr("%1 folders, %2 images, %3 videos").arg(folders).arg(images).arg(videos));
}

QWidget* BrowserWindow::buildTile(QListWidgetItem* item)
{
	const auto& entry = static_cast<const GridEntry&>(*item);
	const auto onWheel = [this](int steps) { zoomTiles(steps); };
	const auto onContextMenu = [this, path = entry.path, isFolder = entry.kind == EntryKind::Folder](QWidget* tile, QPoint pos) {
		showEntryContextMenu(path, isFolder, tile->mapToGlobal(pos));
	};
	const auto asCard = [this, &entry](QWidget* tile) {
		QWidget* card = inSelectionFrame(tile);
		setCutMark(card, isCut(entry.path));
		return card;
	};

	if (entry.kind == EntryKind::Image)
	{
		auto* thumb = new ThumbnailWidget(entry.path, entry.name, _tileSize, nullptr);
		thumb->disableDragGesture();   // the grid drags the whole selection
		thumb->setOnActivatedCallback([this, path = entry.path] { viewImage(path); });
		thumb->setOnMouseWheelCallback(onWheel);
		connect(thumb, &QWidget::customContextMenuRequested, this, [thumb, onContextMenu](QPoint pos) { onContextMenu(thumb, pos); });
		return asCard(thumb);
	}

	auto* tile = new IconTileWidget(_iconProvider.icon(QFileInfo(entry.path)), entry.name, _tileSize, nullptr);
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
	return asCard(tile);
}

void BrowserWindow::zoomTiles(int steps)
{
	const int next = qBound(MIN_TILE_SIZE, _tileSize + steps * TILE_SIZE_STEP, MAX_TILE_SIZE);
	if (next == _tileSize)
		return;

	_tileSize = next;
	QSettings{}.setValue(TILE_SIZE_KEY, next);
	_zoomDebounce->start();
}

void BrowserWindow::applyTileSize()
{
	const QSize cell = cellSize(_tileSize);
	for (int row = 0, rows = _grid->count(); row < rows; ++row)
		_grid->item(row)->setSizeHint(cell);
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
