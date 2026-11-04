#include "BrowserWindow.h"
#include "BatchConvert/BatchConvertDialog.h"
#include "ExplorerContextMenuItem.h"
#include "FileOperations.h"
#include "FileTypeIcons.h"
#include "FolderListing.h"
#include "ImageDecoding/ImageDecoding.h"
#include "RecentPaths.h"
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
#include "utils/naturalsorting/cnaturalsorterqcollator.h"
#include "widgets/chistorycombobox.h"
#include "widgets/clabelelided.h"
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
#include <QGraphicsOpacityEffect>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLineEdit>
#include <QListWidgetItem>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMetaObject>
#include <QMimeData>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QShortcut>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>
RESTORE_COMPILER_WARNINGS

#include <algorithm>
#include <cmath>
#include <memory>
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
const QString RECENT_TRANSFER_FOLDERS_KEY = QStringLiteral("browser/recentTransferFolders");
constexpr int RECENT_TRANSFER_FOLDERS_SIZE = 10;
constexpr int DEFAULT_TILE_WIDTH = 160;
constexpr int MIN_TILE_WIDTH  = 80;
constexpr int MAX_TILE_WIDTH  = 800;
const ThumbnailWidget::Options TILE_OPTIONS{};

// Not QKeySequence::Cut: on Windows and Linux it includes Shift+Del, the permanent deletion.
const QKeySequence CUT_KEYS{ Qt::CTRL | Qt::Key_X };
const QKeySequence COPY_PATHS_KEYS{ Qt::CTRL | Qt::SHIFT | Qt::Key_X };
const QKeySequence RENAME_KEYS{ Qt::Key_F2 };
const QKeySequence COMPARE_KEYS{ Qt::SHIFT | Qt::Key_C };
const QKeySequence BATCH_CONVERT_KEYS{ Qt::SHIFT | Qt::Key_R };

QKeySequence deletionKeys(PathDeletion::Mode mode)
{
	return mode == PathDeletion::Mode::Trash ? QKeySequence{ Qt::Key_Delete } : QKeySequence{ Qt::SHIFT | Qt::Key_Delete };
}

QString transferMenuTitle(TransferKind kind)
{
	return kind == TransferKind::Copy ? QObject::tr("Copy to") : QObject::tr("Move to");
}

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

// The comparison takes two or more images and nothing else.
bool areComparable(const QList<QListWidgetItem*>& items)
{
	const auto isImage = [](const QListWidgetItem* item) { return static_cast<const GridEntry*>(item)->kind == EntryKind::Image; };
	return items.size() >= 2 && std::ranges::all_of(items, isImage);
}

QStringList entryPaths(const QList<QListWidgetItem*>& items)
{
	QStringList paths;
	for (const QListWidgetItem* item : items)
		paths.push_back(static_cast<const GridEntry*>(item)->path);
	return paths;
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

// Paths of the shown images among items, in grid order: a batch conversion numbers clashing names in that order
QStringList imagePathsInGridOrder(const QListWidget& grid, const QList<QListWidgetItem*>& items)
{
	const QSet<const QListWidgetItem*> itemSet{ items.cbegin(), items.cend() };
	QStringList paths;
	for (int row = 0, rows = grid.count(); row < rows; ++row)
	{
		const auto* entry = static_cast<const GridEntry*>(grid.item(row));
		if (entry->kind == EntryKind::Image && !entry->isHidden() && itemSet.contains(entry))
			paths.push_back(entry->path);
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
		if (QListWidgetItem* item = _grid->itemAt(pos))
			showEntryContextMenu(*item, globalPos);
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

	connect(_grid, &QListWidget::itemActivated, this, &BrowserWindow::activateEntry);

	// Not showMessage: hovering a menu item replaces the temporary message with the item's status tip, usually empty.
	// CLabelElided: a QLabel's full text width would set the window's minimum width.
	// ElideRight: both texts lead with their totals.
	_entryCountsLabel = new CLabelElided();
	_entryCountsLabel->setElideMode(Qt::ElideRight);
	statusBar()->addWidget(_entryCountsLabel);
	_selectionSummaryLabel = new CLabelElided();
	_selectionSummaryLabel->setElideMode(Qt::ElideRight);
	statusBar()->addPermanentWidget(_selectionSummaryLabel);
	// Coalesces the per-row selection changes of a bulk row removal into one summary.
	_selectionSummaryDebounce = new QTimer(this);
	_selectionSummaryDebounce->setSingleShot(true);
	_selectionSummaryDebounce->setInterval(0);
	connect(_selectionSummaryDebounce, &QTimer::timeout, this, &BrowserWindow::updateSelectionSummary);
	connect(_grid, &QListWidget::itemSelectionChanged, _selectionSummaryDebounce, qOverload<>(&QTimer::start));

	// Grid-only: the path field keeps its own keys.
	// Enter activates on macOS too: item views there activate with Cmd+O and leave Enter to editing.
	const auto activateCurrent = [this] { activateEntry(_grid->currentItem()); };
	new QShortcut(QKeySequence(Qt::Key_Return), _grid, activateCurrent, Qt::WidgetShortcut);
	new QShortcut(QKeySequence(Qt::Key_Enter), _grid, activateCurrent, Qt::WidgetShortcut);
	for (const PathDeletion::Mode mode : { PathDeletion::Mode::Trash, PathDeletion::Mode::Permanent })
		new QShortcut(deletionKeys(mode), _grid, [this, mode] { deletePaths(selectedPaths(), mode); }, Qt::WidgetShortcut);
	new QShortcut(QKeySequence::Copy, _grid, [this] { FileOperations::putPathsOnClipboard(selectedPaths(), false); }, Qt::WidgetShortcut);
	new QShortcut(CUT_KEYS, _grid, [this] { FileOperations::putPathsOnClipboard(selectedPaths(), true); }, Qt::WidgetShortcut);
	new QShortcut(COPY_PATHS_KEYS, _grid, [this] { FileOperations::putPathsOnClipboardAsText(selectedPaths()); }, Qt::WidgetShortcut);
	new QShortcut(QKeySequence::Paste, _grid, [this] { pasteInto(_currentPath); }, Qt::WidgetShortcut);
	new QShortcut(RENAME_KEYS, _grid, [this] {
		if (const QStringList paths = selectedPaths(); paths.size() == 1)
			renameEntry(paths.front());
	}, Qt::WidgetShortcut);

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
}

void BrowserWindow::setupMainMenu()
{
	QMenu* fileMenu = menuBar()->addMenu(tr("&File"));
	fileMenu->addAction(tr("Settings..."), QKeySequence("Ctrl+Alt+P"), this, [this] { SettingsDialog(this).exec(); });
	fileMenu->addSeparator();
	// Closes every window: video players are top-level and outlive the browser.
	fileMenu->addAction(tr("Exit"), QKeySequence("Ctrl+Q"), this, &QApplication::closeAllWindows);

	QMenu* toolsMenu = menuBar()->addMenu(tr("&Tools"));
	toolsMenu->addAction(tr("Compare photos..."), COMPARE_KEYS, this, [this] {
		comparePhotos(areComparable(_grid->selectedItems()) ? selectedPaths() : QStringList{});
	});
	toolsMenu->addAction(tr("Resize / convert images..."), BATCH_CONVERT_KEYS, this, [this] {
		BatchConvertDialog::showForImages(imagePathsInGridOrder(*_grid, _grid->selectedItems()));
	});

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
	QString absolutePath = dir.absolutePath();
	// Before the listing: a change made before the watch starts may go unreported.
	auto watcher = std::make_unique<FolderWatcher>(absolutePath, [this] {
		if (!_refreshDebounce->isActive())
			_refreshDebounce->start();
	});
	std::optional<std::vector<FolderEntry>> entries = listFolder(absolutePath);
	if (!entries)
		return false;

	_grid->clear();
	_currentPath = std::move(absolutePath);
	_folderWatcher = std::move(watcher);

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
	case EntryKind::Folder:
		// Queued: navigating rebuilds the grid, deleting the row and tile whose activation is still being handled.
		QMetaObject::invokeMethod(this, [this, path = entry.path] { navigateTo(path); }, Qt::QueuedConnection);
		break;
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

void BrowserWindow::showEntryContextMenu(QListWidgetItem& item, QPoint globalPos)
{
	const auto& entry = static_cast<const GridEntry&>(item);
	const QString path = entry.path;
	QMenu menu(this);
	menu.addAction(revealInFileManagerActionText(), this, [this, path] {
		if (!revealInFileManager(path))
			reportMissingFile(this, path);
	});

	const QList<QListWidgetItem*> targetItems = MediaGrid::effectiveSelection(item);
	const QStringList targets = entryPaths(targetItems);

	const bool comparable = areComparable(targetItems);
	const QStringList targetImages = imagePathsInGridOrder(*_grid, targetItems);
	if (comparable || !targetImages.empty())
		menu.addSeparator();
	if (comparable)
		menu.addAction(tr("Compare photos"), COMPARE_KEYS, this, [this, targets] { comparePhotos(targets); });
	if (!targetImages.empty())
		menu.addAction(tr("Resize / convert..."), BATCH_CONVERT_KEYS, this, [targetImages] { BatchConvertDialog::showForImages(targetImages); });

	menu.addSeparator();
	menu.addAction(tr("Cut"), CUT_KEYS, this, [targets] { FileOperations::putPathsOnClipboard(targets, true); });
	menu.addAction(tr("Copy"), QKeySequence::Copy, this, [targets] { FileOperations::putPathsOnClipboard(targets, false); });
	const QString copyPathsText = targets.size() == 1 ? tr("Copy full path") : tr("Copy full paths");
	menu.addAction(copyPathsText, COPY_PATHS_KEYS, this, [targets] { FileOperations::putPathsOnClipboardAsText(targets); });
	addTransferSubmenu(menu, TransferKind::Copy, targets);
	addTransferSubmenu(menu, TransferKind::Move, targets);
	if (entry.kind == EntryKind::Folder)
		menu.addAction(tr("Paste into folder"), this, [this, path] { pasteInto(path); })->setEnabled(clipboardHasFiles());

	// Rename and deletion are queued, like folder activation in activateEntry: the tile showing this menu may be among those removed.
	const auto addDeleteAction = [this, &menu, targets](PathDeletion::Mode mode) {
		menu.addAction(FileOperations::deleteActionText(mode), deletionKeys(mode), this, [this, targets, mode] {
			QMetaObject::invokeMethod(this, [this, targets, mode] { deletePaths(targets, mode); }, Qt::QueuedConnection);
		});
	};
	menu.addSeparator();
	if (targets.size() == 1)
	{
		menu.addAction(tr("Rename..."), RENAME_KEYS, this, [this, path] {
			QMetaObject::invokeMethod(this, [this, path] { renameEntry(path); }, Qt::QueuedConnection);
		});
	}
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
	menu.addAction(tr("Paste"), QKeySequence::Paste, this, [this] { pasteInto(_currentPath); })->setEnabled(clipboardHasFiles());
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

void BrowserWindow::addTransferSubmenu(QMenu& menu, TransferKind kind, const QStringList& sources)
{
	QMenu* submenu = menu.addMenu(transferMenuTitle(kind));
	const QStringList recentFolders = RecentPaths::read(RECENT_TRANSFER_FOLDERS_KEY);
	const QString currentFolderKey = pathComparisonKey(_currentPath);
	for (qsizetype index = 0; index < recentFolders.size(); ++index)
	{
		const QString& folder = recentFolders[index];
		QAction* action = submenu->addAction(RecentPaths::menuEntryText(index, folder), this, [this, kind, sources, folder] { transferInto(kind, sources, folder); });
		// Disabled, not left out: the other entries keep their numbers.
		action->setEnabled(pathComparisonKey(folder) != currentFolderKey);
	}

	if (!recentFolders.empty())
		submenu->addSeparator();
	const QString startFolder = recentFolders.empty() ? _currentPath : recentFolders.front();
	submenu->addAction(tr("Choose folder..."), this, [this, kind, sources, startFolder] {
		if (const QString folder = QFileDialog::getExistingDirectory(this, transferMenuTitle(kind), startFolder); !folder.isEmpty())
			transferInto(kind, sources, folder);
	});
}

void BrowserWindow::transferInto(TransferKind kind, const QStringList& sources, const QString& folder)
{
	// Not checked when the menu is built: the check can block on an unavailable network share.
	if (!QFileInfo(folder).isDir())
	{
		const QString question = tr("The folder cannot be found:\n%1\n\nRemove it from the recently used folders?").arg(QDir::toNativeSeparators(folder));
		if (QMessageBox::question(this, transferMenuTitle(kind), question) == QMessageBox::Yes)
			RecentPaths::remove(RECENT_TRANSFER_FOLDERS_KEY, folder);
		return;
	}

	RecentPaths::record(RECENT_TRANSFER_FOLDERS_KEY, folder, RECENT_TRANSFER_FOLDERS_SIZE);
	FileOperations::startTransfer(kind, sources, folder, this);
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

Qt::DropAction BrowserWindow::dropAction(const QDropEvent& event, const QString& source, const QString& targetFolder)
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
	if (!copyKey && !moveKey && targetFolder != _volumeCheckedDropFolder)
	{
		_volumeCheckedDropFolder = targetFolder;
		_dropFolderOnSourceVolume = FileOperations::isOnSameVolume(source, targetFolder);
	}
	const bool move = !copyKey && (moveKey || _dropFolderOnSourceVolume);
	const Qt::DropAction preferred = move ? Qt::MoveAction : Qt::CopyAction;
	if (event.possibleActions().testFlag(preferred))
		return preferred;
	return event.possibleActions().testFlag(Qt::CopyAction) ? Qt::CopyAction : Qt::IgnoreAction;
}

void BrowserWindow::dragEnterEvent(QDragEnterEvent* event)
{
	if (FileOperations::localFilePaths(*event->mimeData()).empty())
		return;

	_volumeCheckedDropFolder.clear();
	dragMoveEvent(event);
}

void BrowserWindow::dragMoveEvent(QDragMoveEvent* event)
{
	const QStringList sources = FileOperations::localFilePaths(*event->mimeData());
	const QString target = dropTargetFolder(*event, sources);
	const Qt::DropAction action = sources.empty() || target.isEmpty() ? Qt::IgnoreAction : dropAction(*event, sources.front(), target);
	if (action == Qt::IgnoreAction)
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
	const Qt::DropAction action = sources.empty() || target.isEmpty() ? Qt::IgnoreAction : dropAction(*event, sources.front(), target);
	if (action == Qt::IgnoreAction)
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
	return entryPaths(_grid->selectedItems());
}

void BrowserWindow::renameEntry(const QString& path)
{
	const QString newPath = FileOperations::renameInteractive(path, this);
	if (newPath.isEmpty())
		return;

	refresh();
	selectAndScrollToPath(newPath);
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

	QString counts = tr("%1 folder(s), %2 image(s), %3 video(s)").arg(folders).arg(images).arg(videos);
	if (hidden > 0)
		counts += tr(", %1 hidden by filter").arg(hidden);
	_entryCountsLabel->setText(counts);
	statusBar()->clearMessage(); // ends a navigation error message, which hides the counts
	_grid->setEmptyMessage(hidden > 0 ? tr("No names match the filter.") : tr("No folders or supported media files here."));
}

void BrowserWindow::updateSelectionSummary()
{
	const QList<QListWidgetItem*> selection = _grid->selectedItems();
	if (selection.empty())
	{
		_selectionSummaryLabel->clear();
		return;
	}

	int folders = 0, files = 0;
	std::unordered_map<QString, int> filesPerExtension;
	for (const QListWidgetItem* item : selection)
	{
		const auto& entry = static_cast<const GridEntry&>(*item);
		if (entry.kind == EntryKind::Folder)
		{
			++folders;
			continue;
		}
		++files;
		++filesPerExtension[QFileInfo(entry.name).suffix().toUpper()];
	}

	std::vector<std::pair<QString, int>> extensionCounts(filesPerExtension.begin(), filesPerExtension.end());
	std::ranges::sort(extensionCounts, NaturalSort::lessCaseSensitive, &std::pair<QString, int>::first);
	QStringList extensionTexts;
	for (const auto& [extension, count] : extensionCounts)
		extensionTexts.push_back(QStringLiteral("%1: %2").arg(extension).arg(count));

	QString summary = tr("Selected: %1 folder(s), %2 file(s)").arg(folders).arg(files);
	if (!extensionTexts.empty())
		summary += QStringLiteral(" (%1)").arg(extensionTexts.join(QStringLiteral(", ")));
	_selectionSummaryLabel->setText(summary);
}

QWidget* BrowserWindow::buildTile(QListWidgetItem* item)
{
	const auto& entry = static_cast<const GridEntry&>(*item);
	const QFileInfo fileInfo(entry.path);
	const QByteArray videoType = entry.kind == EntryKind::Video ? fileInfo.suffix().toLower().toLatin1() : QByteArray{};
	ThumbnailWidget::Source source = entry.kind == EntryKind::Image
		? ThumbnailWidget::Source{ ImageFilesSource{ QStringList{ entry.path } } }
		: ThumbnailWidget::Source{ IconSource{ _iconProvider.icon(fileInfo), videoType } };

	// No drag or activation of its own: the grid drags the selection and activates the row.
	auto* tile = new ThumbnailWidget(std::move(source), tileCaption(entry), TILE_OPTIONS, nullptr);
	tile->setFixedSize(tileSize());
	tile->setFileTypeIconLookup(&FileTypeIcons::forType);
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
