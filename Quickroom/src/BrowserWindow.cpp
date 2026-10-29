#include "BrowserWindow.h"
#include "ExplorerContextMenuItem.h"
#include "FileOperations.h"
#include "FileTypeIcons.h"
#include "FolderListing.h"
#include "ImageDecoding/ImageDecoding.h"
#include "SettingsDialog.h"
#include "Theme/Theme.h"
#include "UiComponents/IconSource.h"
#include "UiComponents/ImageFilesSource.h"
#include "UiComponents/MediaGrid.h"
#include "UiComponents/NameFilterField.h"
#include "UiComponents/SortControl.h"
#include "UiComponents/ThumbnailWidget.h"
#include "Windows/ImageViewerWindow.h"
#include "Windows/OpenFileRelease.h"
#include "Windows/PhotoCompareWindow.h"
#include "Windows/VideoPlayerWindow.h"
#include "Utils.h"
#include "appdialogs/caboutdialog.h"
#include "appdialogs/reportbugdialog.h"
#include "assert/advanced_assert.h"
#include "compiler/compiler_warnings_control.h"
#include "logger/cloggerinmemory.h"
#include "utils/filemanager.h"
#include "widgets/chistorycombobox.h"
#include "widgets/cpersistenceenabler.h"

DISABLE_COMPILER_WARNINGS
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QGraphicsOpacityEffect>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLineEdit>
#include <QListWidgetItem>
#include <QMenu>
#include <QMenuBar>
#include <QMetaObject>
#include <QMimeData>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QShortcut>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStorageInfo>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>
RESTORE_COMPILER_WARNINGS

#include <algorithm>
#include <cmath>
#include <optional>
#include <ranges>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

const QString LAST_FOLDER_KEY = QStringLiteral("browser/lastFolder");
const QString TILE_SIZE_KEY   = QStringLiteral("browser/tileSize");
const QString PATH_FIELD_HISTORY_KEY = QStringLiteral("browser/pathFieldHistory");
constexpr int PATH_FIELD_HISTORY_SIZE = 100;
constexpr int DEFAULT_TILE_WIDTH = 160;
constexpr int MIN_TILE_WIDTH  = 80;
constexpr int MAX_TILE_WIDTH  = 800;
const ThumbnailWidget::Options TILE_OPTIONS{};

int savedTileWidth()
{
	return qBound(MIN_TILE_WIDTH, QSettings{}.value(TILE_SIZE_KEY, DEFAULT_TILE_WIDTH).toInt(), MAX_TILE_WIDTH);
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

class GridEntry final : public QListWidgetItem, public FolderEntry {
public:
	GridEntry(FolderEntry entry, const SortOrder& order) : FolderEntry{ std::move(entry) }, _order{ &order } {}

	bool operator<(const QListWidgetItem& other) const override
	{
		return precedesInOrder(*this, static_cast<const GridEntry&>(other), *_order);
	}

private:
	// Not shared: each browser window has its own sort control.
	const SortOrder* _order;
};

// The row that keeps the sorted grid sorted with entry inserted.
int sortedInsertionRow(const QListWidget& grid, const GridEntry& entry)
{
	return *std::ranges::partition_point(std::views::iota(0, grid.count()), [&](int row) { return *grid.item(row) < entry; });
}

// Paths of the shown entries of kind, in grid order.
QStringList shownPathsOfKind(const QListWidget& grid, EntryKind kind)
{
	QStringList paths;
	for (int row = 0, rows = grid.count(); row < rows; ++row)
	{
		const auto& entry = static_cast<const GridEntry&>(*grid.item(row));
		if (entry.kind == kind && !entry.isHidden())
			paths.push_back(entry.path);
	}
	return paths;
}

// The first shown item at or after row, else the last shown one before it.
QListWidgetItem* nearestShownItem(const QListWidget& grid, int row)
{
	for (int next = row, rows = grid.count(); next < rows; ++next)
		if (!grid.item(next)->isHidden())
			return grid.item(next);
	for (int previous = row - 1; previous >= 0; --previous)
		if (!grid.item(previous)->isHidden())
			return grid.item(previous);
	return nullptr;
}

// A file's name without its extension: the tile's type icon stands in for it. A name that is all extension stays whole.
QString tileCaption(const FolderEntry& entry)
{
	if (entry.kind == EntryKind::Folder)
		return entry.name;
	const QString baseName = QFileInfo(entry.name).completeBaseName();
	return baseName.isEmpty() ? entry.name : baseName;
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

void BrowserWindow::showForFolder(const QString& folder, const QString& selectPath, PathFieldHistory history)
{
	auto* window = new BrowserWindow(resolveStartFolder(folder), history);
	window->setAttribute(Qt::WA_DeleteOnClose);
	window->show();
	// Scrolling to the item needs the laid-out viewport the window only has once shown.
	if (!selectPath.isEmpty())
		window->selectAndScrollToPath(selectPath);
}

BrowserWindow::BrowserWindow(const QString& folder, PathFieldHistory history)
	: _tileWidth{ savedTileWidth() }
{
	setupUi();
	setupMainMenu();

	resize(1200, 800);
	enablePersistence(this, "browser/window", CPersistenceEnabler::Delayed{ true }, CPersistenceEnabler::SetDefaultSize{ false });

	navigateTo(folder, history);
}

void BrowserWindow::setupUi()
{
	auto* header = new QWidget();
	auto* headerLayout = new QHBoxLayout(header);
	headerLayout->setContentsMargins(4, 4, 4, 4);
	headerLayout->setSpacing(4);

	// The actions belong to the window, so their shortcuts work wherever the focus is.
	const int navigationIconSize = style()->pixelMetric(QStyle::PM_ToolBarIconSize);
	const auto addNavigationButton = [&](QStyle::StandardPixmap icon, const QString& text, void (BrowserWindow::*navigate)()) {
		QAction* action = addAction(style()->standardIcon(icon), text, this, navigate);
		auto* button = new QToolButton();
		button->setDefaultAction(action);
		button->setAutoRaise(true);
		button->setFocusPolicy(Qt::NoFocus);
		button->setIconSize({ navigationIconSize, navigationIconSize });
		headerLayout->addWidget(button);
		return action;
	};
	_backAction = addNavigationButton(QStyle::SP_ArrowBack, tr("Back"), &BrowserWindow::goBack);
	_backAction->setShortcuts(QKeySequence::Back);
	_forwardAction = addNavigationButton(QStyle::SP_ArrowForward, tr("Forward"), &BrowserWindow::goForward);
	_forwardAction->setShortcuts(QKeySequence::Forward);
	_upAction = addNavigationButton(QStyle::SP_ArrowUp, tr("Up"), &BrowserWindow::goUp);
	_upAction->setShortcut(QKeySequence(Qt::Key_Backspace));

	_pathField = new CHistoryComboBox(header);
	// Not sized by its items: a long path in the history would set the window's minimum width
	_pathField->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	_pathField->setMaxCount(PATH_FIELD_HISTORY_SIZE);
	_pathField->setSettingsKeyForHistoryPersistence(PATH_FIELD_HISTORY_KEY);
	const auto navigateToPathFieldText = [this](const QString& text) {
		navigateTo(QDir::fromNativeSeparators(text.trimmed()), PathFieldHistory::Record);
	};
	connect(_pathField, &CHistoryComboBox::itemActivated, this, navigateToPathFieldText);
	// Queued: navigating rebuilds the items while the combo box is still handling the pick
	connect(_pathField, &QComboBox::textActivated, this, navigateToPathFieldText, Qt::QueuedConnection);
	headerLayout->addWidget(_pathField, 1);

	auto* browseButton = new QPushButton(tr("Browse..."));
	browseButton->setFocusPolicy(Qt::NoFocus);
	connect(browseButton, &QPushButton::clicked, this, [this] {
		if (const QString folder = QFileDialog::getExistingDirectory(this, tr("Open folder"), _currentPath); !folder.isEmpty())
			navigateTo(folder, PathFieldHistory::Record);
	});
	headerLayout->addWidget(browseButton);
	headerLayout->addSpacing(8);

	_nameFilter = new NameFilterField();
	connect(_nameFilter, &QLineEdit::textChanged, this, &BrowserWindow::applyNameFilter);
	headerLayout->addWidget(_nameFilter);
	new QShortcut(QKeySequence::Find, this, [this] {
		_nameFilter->setFocus();
		_nameFilter->selectAll();
	});

	_sortControl = new SortControl(BROWSER_SORT_SETTINGS_GROUP, {}, BROWSER_SORT_OPTIONS);
	connect(_sortControl, &SortControl::changed, this, [this] {
		_grid->sortItems(Qt::AscendingOrder);
		_grid->ensureVisibleCardsExist();
	});
	headerLayout->addWidget(_sortControl);

	auto* refreshShortcut = new QShortcut(QKeySequence::Refresh, this);
	connect(refreshShortcut, &QShortcut::activated, this, &BrowserWindow::refresh);

	_grid = new MediaGrid();
	_grid->setObjectName("tileGrid");   // the central sheet leaves this grid's cells unpainted, replacing MediaGrid's own name
	_grid->setSpacing(Theme::current().metrics.tileGridSpacing);
	_grid->setUniformItemSizes(true);
	_grid->setDragEnabled(true);
	_grid->setCardFactory([this](QListWidgetItem* item) { return buildTile(item); });
	_grid->setZoomHandler([this](int steps) { zoomTiles(steps); });
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

	auto* central = new QWidget();
	auto* centralLayout = new QVBoxLayout(central);
	centralLayout->setContentsMargins(0, 0, 0, 0);
	centralLayout->setSpacing(0);
	centralLayout->addWidget(header);
	centralLayout->addWidget(_grid);
	setCentralWidget(central);
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

void BrowserWindow::setupMainMenu()
{
	QMenu* fileMenu = menuBar()->addMenu(tr("&File"));
	fileMenu->addAction(tr("Settings..."), QKeySequence("Ctrl+Alt+P"), this, [this] { SettingsDialog(this).exec(); });
	fileMenu->addSeparator();
	// Closes every window: video players are top-level and outlive the browser.
	fileMenu->addAction(tr("Exit"), QKeySequence("Ctrl+Q"), this, &QApplication::closeAllWindows);

	QMenu* toolsMenu = menuBar()->addMenu(tr("&Tools"));
	toolsMenu->addAction(tr("Compare photos..."), QKeySequence("Shift+C"), this, [this] { comparePhotos({}); });

	QMenu* helpMenu = menuBar()->addMenu(tr("&Help"));
	helpMenu->addAction(tr("Report a bug..."), this, [this] {
		ReportBugDialog::show(this, loggerInstance<CLoggerInMemory>(), QUrl(IssueTrackerUrl));
	});
	helpMenu->addSeparator();
	helpMenu->addAction(tr("About Quickroom..."), this, [this] {
		CAboutDialog(QApplication::applicationVersion(), this).exec();
	});
}

bool BrowserWindow::listDirectory(const QString& path)
{
	const QDir dir(path);
	std::optional<std::vector<FolderEntry>> entries = listFolder(dir.absolutePath());
	if (!entries)
		return false;

	_grid->clear();
	_currentPath = dir.absolutePath();

	if (const QStringList watched = _folderWatcher->directories(); !watched.empty())
		_folderWatcher->removePaths(watched);
	_folderWatcher->addPath(_currentPath);

	for (FolderEntry& entry : *entries)
		_grid->addItem(new GridEntry(std::move(entry), _sortControl->order()));
	_grid->sortItems(Qt::AscendingOrder);

	const QStringList images = shownPathsOfKind(*_grid, EntryKind::Image);
	_firstImageSize = images.empty() ? QSize{} : ImageDecoding::readUprightImageSize(images.front());
	setTileSizeHints();
	_grid->scrollToTop();
	applyNameFilter();

	const QString nativePath = QDir::toNativeSeparators(_currentPath);
	_pathField->setCurrentIndex(_pathField->findText(nativePath)); // the dropdown highlights the listed folder, or nothing
	_pathField->setEditText(nativePath);
	setWindowTitle((dir.dirName().isEmpty() ? nativePath : dir.dirName()) + QStringLiteral(" - Quickroom"));
	QSettings{}.setValue(LAST_FOLDER_KEY, _currentPath);
	return true;
}

void BrowserWindow::navigateTo(const QString& path, PathFieldHistory history)
{
	const bool samePath = _historyIndex >= 0 && _history[static_cast<size_t>(_historyIndex)] == QDir(path).absolutePath();
	if (!listDirectory(path))
	{
		statusBar()->showMessage(tr("Cannot open \"%1\"").arg(QDir::toNativeSeparators(path)));
		_pathField->setEditText(QDir::toNativeSeparators(_currentPath));
		return;
	}
	if (history == PathFieldHistory::Record)
		_pathField->addToHistory(QDir::toNativeSeparators(_currentPath));
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
	if (!QDir(_currentPath).exists())
	{
		navigateTo(resolveStartFolder(nearestExistingFolder(_currentPath)));
		return;
	}

	std::optional<std::vector<FolderEntry>> listing = listFolder(_currentPath);
	if (!listing)
		return;

	std::unordered_map<QString, FolderEntry> onDisk;
	for (FolderEntry& entry : *listing)
	{
		QString path = entry.path;
		onDisk.emplace(std::move(path), std::move(entry));
	}

	// Afterwards onDisk holds only the entries the grid lacks.
	bool changed = false;
	bool datesChanged = false;
	for (int row = _grid->count() - 1; row >= 0; --row)
	{
		auto* shown = static_cast<GridEntry*>(_grid->item(row));
		const auto found = onDisk.find(shown->path);
		if (found == onDisk.end() || found->second.kind != shown->kind)
		{
			delete _grid->takeItem(row);
			changed = true;
			continue;
		}

		const FolderEntry& current = found->second;
		// A file rewritten in place needs a new tile.
		if (shown->kind != EntryKind::Folder && (current.size != shown->size || current.lastModified != shown->lastModified))
		{
			_grid->removeItemWidget(shown);
			changed = true;
		}
		datesChanged = datesChanged || current.lastModified != shown->lastModified;
		shown->size = current.size;
		shown->lastModified = current.lastModified;
		onDisk.erase(found);
	}

	// Before the insertions below: they require a sorted grid.
	if (datesChanged && _sortControl->order().sortBy == SortBy::Date)
	{
		_grid->sortItems(Qt::AscendingOrder);
		changed = true;
	}

	const QSize cell = tileSize();
	for (auto& [path, entry] : onDisk)
	{
		auto* item = new GridEntry(std::move(entry), _sortControl->order());
		item->setSizeHint(cell);
		_grid->insertItem(sortedInsertionRow(*_grid, *item), item);
		changed = true;
	}

	if (changed)
		applyNameFilter();
}

void BrowserWindow::applyNameFilter()
{
	_grid->filterRows([this](const QListWidgetItem& item) { return _nameFilter->matches(static_cast<const GridEntry&>(item).name); });
	_grid->ensureVisibleCardsExist();
	updateCountsAndEmptyMessage();
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
	const QStringList imagePaths = shownPathsOfKind(*_grid, EntryKind::Image);
	const int startIndex = static_cast<int>(imagePaths.indexOf(path));
	assert_and_return_r(startIndex >= 0, );

	ImageViewerWindow* const viewer = ImageViewerWindow::showForImages(imagePaths, startIndex, this, pathSelector());
	if (!viewer)
		return;

	FileOperations::installDeleteActions(*viewer, entryRemover());
}

void BrowserWindow::playVideo(const QString& path)
{
	if (!QFile::exists(path))
	{
		reportMissingFile(this, path);
		return;
	}

	VideoPlayerWindow* const player = VideoPlayerWindow::createPlayerWindow(path, nullptr);
	FileOperations::installDeleteActions(*player, entryRemover());
	player->setNavigation(shownPathsOfKind(*_grid, EntryKind::Video), pathSelector());
}

void BrowserWindow::comparePhotos(const QStringList& paths)
{
	// Parentless, for a taskbar button of its own.
	if (PhotoCompareWindow* const compareWindow = PhotoCompareWindow::showForFiles(paths, nullptr))
		FileOperations::installDeleteAction(*compareWindow, entryRemover());
}

std::function<void(const QString& path)> BrowserWindow::entryRemover()
{
	return [self = QPointer<BrowserWindow>{ this }](const QString& path) {
		if (self)
			self->removeEntries({ path });
	};
}

std::function<void(const QString& path)> BrowserWindow::pathSelector()
{
	return [self = QPointer<BrowserWindow>{ this }](const QString& path) {
		if (self)
			self->selectAndScrollToPath(path);
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

	// Several targets are the selection.
	const auto isImage = [](const QListWidgetItem* item) { return static_cast<const GridEntry*>(item)->kind == EntryKind::Image; };
	if (targets.size() >= 2 && std::ranges::all_of(_grid->selectedItems(), isImage))
	{
		menu.addSeparator();
		menu.addAction(tr("Compare photos"), this, [this, targets] { comparePhotos(targets); });
	}

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

#ifdef Q_OS_WIN
	menu.addSeparator();
	addExplorerContextMenuItem(menu, targets, *this);
#endif

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
		if (item->isHidden() || static_cast<const GridEntry*>(item)->path != path)
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

	if (QListWidgetItem* next = nearestShownItem(*_grid, rowAfterLastRemoved))
		_grid->setCurrentItem(next);
	_grid->ensureVisibleCardsExist();
	updateCountsAndEmptyMessage();
}

void BrowserWindow::updateCountsAndEmptyMessage()
{
	int folders = 0, images = 0, videos = 0, hidden = 0;
	for (int row = 0, rows = _grid->count(); row < rows; ++row)
	{
		const auto& entry = static_cast<const GridEntry&>(*_grid->item(row));
		if (entry.isHidden())
		{
			++hidden;
			continue;
		}
		switch (entry.kind)
		{
		case EntryKind::Folder: ++folders; break;
		case EntryKind::Image:  ++images;  break;
		case EntryKind::Video:  ++videos;  break;
		}
	}

	QString counts = tr("%1 folders, %2 images, %3 videos").arg(folders).arg(images).arg(videos);
	if (hidden > 0)
		counts += tr(", %1 hidden by filter").arg(hidden);
	statusBar()->showMessage(counts);
	_grid->setEmptyMessage(hidden > 0 ? tr("No names match the filter.") : tr("No folders or supported media files here."));
}

QWidget* BrowserWindow::buildTile(QListWidgetItem* item)
{
	const auto& entry = static_cast<const GridEntry&>(*item);
	const QFileInfo fileInfo(entry.path);
	const QByteArray videoType = entry.kind == EntryKind::Video ? fileInfo.suffix().toLower().toLatin1() : QByteArray{};
	ThumbnailWidget::Source source = entry.kind == EntryKind::Image
		? ThumbnailWidget::Source{ ImageFilesSource{ QStringList{ entry.path } } }
		: ThumbnailWidget::Source{ IconSource{ _iconProvider.icon(fileInfo), videoType } };

	// No drag of its own: the grid drags the whole selection.
	auto* tile = new ThumbnailWidget(std::move(source), tileCaption(entry), TILE_OPTIONS, nullptr);
	tile->setFixedSize(tileSize());
	switch (entry.kind)
	{
	case EntryKind::Folder:
		// Queued: navigating rebuilds the grid, which would delete this tile mid-handler.
		tile->setOnActivatedCallback([this, path = entry.path] {
			QMetaObject::invokeMethod(this, [this, path] { navigateTo(path); }, Qt::QueuedConnection);
		});
		break;
	case EntryKind::Image: tile->setOnActivatedCallback([this, path = entry.path] { viewImage(path); }); break;
	case EntryKind::Video: tile->setOnActivatedCallback([this, path = entry.path] { playVideo(path); }); break;
	}
	tile->setFileTypeIconLookup(&FileTypeIcons::forType);
	tile->setContextMenuPolicy(Qt::CustomContextMenu);
	connect(tile, &QWidget::customContextMenuRequested, this, [this, tile, path = entry.path, isFolder = entry.kind == EntryKind::Folder](QPoint pos) {
		showEntryContextMenu(path, isFolder, tile->mapToGlobal(pos));
	});
	setCutMark(tile, isCut(entry.path));
	return tile;
}

QSize BrowserWindow::tileSize() const
{
	if (_firstImageSize.isEmpty())
		return { _tileWidth, _tileWidth };

	// Bounded: a panorama or a tall strip would squeeze every other tile's image
	const double heightPerWidth = std::clamp(static_cast<double>(_firstImageSize.height()) / _firstImageSize.width(), 0.5, 2.0);
	const QSize chrome = ThumbnailWidget::chromeSize(TILE_OPTIONS, true); // tileCaption is never empty
	return { _tileWidth, chrome.height() + static_cast<int>(std::lround((_tileWidth - chrome.width()) * heightPerWidth)) };
}

void BrowserWindow::setTileSizeHints()
{
	const QSize cell = tileSize();
	for (int row = 0, rows = _grid->count(); row < rows; ++row)
		_grid->item(row)->setSizeHint(cell);
}

void BrowserWindow::zoomTiles(int steps)
{
	const int next = MediaGrid::zoomedSize(_tileWidth, steps, MIN_TILE_WIDTH, MAX_TILE_WIDTH);
	if (next == _tileWidth)
		return;

	_tileWidth = next;
	QSettings{}.setValue(TILE_SIZE_KEY, next);
	_zoomDebounce->start();
}

void BrowserWindow::applyTileSize()
{
	setTileSizeHints();
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
