#include "Windows/MainWindow.h"
#include "Core/Catalog.h"
#include "Core/Library.h"
#include "Windows/CompareWindow.h"
#include "Windows/PhotoCompareWindow.h"
#include "Ffmpeg.h"
#include "Import.h"
#include "Windows/FrameViewerWindow.h"
#include "Windows/IntegrityCheckDialog.h"
#include "UiComponents/LabelSidebar.h"
#include "UiComponents/LabelVisuals.h"
#include "Windows/ImportDialog.h"
#include "Windows/LogViewerDialog.h"
#include "Windows/MediaRename.h"
#include "UiComponents/SegmentedToggle.h"
#include "Windows/SettingsDialog.h"
#include "UiComponents/SortControl.h"
#include "Core/MediaId.h"
#include "UiComponents/MediaGrid.h"
#include "UiComponents/MediaItemWidget.h"
#include "Windows/VideoPlayerWindow.h"
#include "Theme/Icons.h"
#include "Theme/Style.h"
#include "Theme/Theme.h"
#include "Utils.h"
#include "Settings.h"
#include "Shortcuts.h"

#include "aboutdialog/caboutdialog.h"
#include "assert/advanced_assert.h"
#include "dialogs/messagebox.h"
#include "utils/naturalsorting/cnaturalsorterqcollator.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QColor>
#include <QColorDialog>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QScrollBar>
#include <QSet>
#include <QSettings>
#include <QShortcut>
#include <QSplitter>
#include <QStandardPaths>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>

#include <QScopeGuard>

#include <algorithm>
#include <functional>
#include <utility>
#include <vector>

namespace {
// QComboBox paints the current item's icon itself; popup rows should remain undecorated.
class ClosedControlIconDelegate final : public QStyledItemDelegate {
public:
	using QStyledItemDelegate::QStyledItemDelegate;
	void initStyleOption(QStyleOptionViewItem* option, const QModelIndex& index) const override
	{
		QStyledItemDelegate::initStyleOption(option, index);
		option->features &= ~QStyleOptionViewItem::HasDecoration;
		option->icon = QIcon();
	}
};
}

// Card width scales with this per-frame height.
static const QString CARD_IMAGE_HEIGHT_KEY = "mainWindow/cardImageHeight";
static const QString MEDIA_TYPE_FILTER_KEY = "mainWindow/mediaTypeFilter";
static constexpr int DEFAULT_CARD_IMAGE_HEIGHT = 120;
static constexpr int MIN_CARD_IMAGE_HEIGHT = 60;
static constexpr int MAX_CARD_IMAGE_HEIGHT = 360;
static constexpr int CARD_IMAGE_HEIGHT_STEP = 20;

static constexpr int MIN_PREVIEW_FRAME_COUNT = 1;
static constexpr int MAX_PREVIEW_FRAME_COUNT = 10;

inline bool    useTiff()     { return QSettings{}.value(Settings::UseTiff,      Defaults::UseTiff).toBool(); }
inline int     jpegQuality() { return QSettings{}.value(Settings::JpegQuality,  Defaults::JpegQuality).toInt(); }
inline int     frameStep()   { return QSettings{}.value(Settings::FrameStep,    Defaults::FrameStep).toInt(); }
inline int     cardImageHeight() { return QSettings{}.value(CARD_IMAGE_HEIGHT_KEY, DEFAULT_CARD_IMAGE_HEIGHT).toInt(); }

namespace {

QString libraryPickerStartFolder(const QString& path)
{
	const QFileInfo info(path);
	if (info.isDir())
		return info.absoluteFilePath();
	const QString parent = info.absolutePath();
	return QDir(parent).exists() ? parent : QDir::homePath();
}

// The window is not yet displayable, so the first-run dialogs are intentionally unparented.
[[nodiscard]] QString chooseFirstRunLibraryFolder()
{
	QString documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
	if (documents.isEmpty())
		documents = QDir::homePath();
	const QString suggested = QDir(documents).filePath(QStringLiteral("Darkroom"));

	QMessageBox box(QMessageBox::Information, QObject::tr("Welcome to Darkroom"),
		QObject::tr("Darkroom keeps your photos, extracted frames and catalog together in one library folder.\n\n"
			"Suggested location:\n%1\n\nUse this folder, or choose a different one.").arg(QDir::toNativeSeparators(suggested)));
	QPushButton* useSuggested = box.addButton(QObject::tr("Use This Folder"), QMessageBox::AcceptRole);
	QPushButton* chooseOther  = box.addButton(QObject::tr("Choose Folder..."), QMessageBox::ActionRole);
	box.addButton(QObject::tr("Quit"), QMessageBox::RejectRole);
	box.setDefaultButton(useSuggested);
	box.exec();

	if (box.clickedButton() == useSuggested)
		return suggested;
	if (box.clickedButton() == chooseOther)
		return QFileDialog::getExistingDirectory(nullptr, QObject::tr("Choose library folder"), libraryPickerStartFolder(suggested));
	return {};
}

constexpr int MAX_RECENT_LIBRARIES = 8;

// Do not stat recent roots: an entry may refer to an unavailable drive or network share.
[[nodiscard]] QStringList recentLibraries()
{
	return QSettings{}.value(Settings::RecentLibraries).toStringList();
}

void recordCurrentLibrary(const QString& root)
{
	QSettings settings;
	settings.setValue(Settings::RootFolder, root);

	QStringList recents = settings.value(Settings::RecentLibraries).toStringList();
	recents.removeIf([&root](const QString& entry) { return entry.compare(root, Qt::CaseInsensitive) == 0; });
	recents.prepend(root);
	if (recents.size() > MAX_RECENT_LIBRARIES)
		recents.resize(MAX_RECENT_LIBRARIES);
	settings.setValue(Settings::RecentLibraries, recents);
}

[[nodiscard]] bool deleteFileIfPresent(const QString& filePath)
{
	if (filePath.isEmpty())
		return false;

	const QFileInfo info(filePath);
	return (!info.exists() && !info.isSymLink()) || QFile::remove(filePath);
}

[[nodiscard]] bool deleteFolderRecursivelyIfPresent(const QString& folderPath)
{
	if (folderPath.isEmpty())
		return false;

	const QFileInfo info(folderPath);
	if (!info.exists() && !info.isSymLink())
		return true;
	if (!info.isDir())
		return false;
	return QDir(folderPath).removeRecursively();
}

} // namespace

bool MainWindow::loadInitialLibrary()
{
	QSettings settings;
	QString requestedRoot;
	if (settings.contains(Settings::RootFolder))
	{
		requestedRoot = settings.value(Settings::RootFolder).toString();
	}
	else
	{
		requestedRoot = chooseFirstRunLibraryFolder();
		if (requestedRoot.isEmpty())
			return false;
	}

	for(;;)
	{
		QString error;
		if (_library.setRoot(requestedRoot, &error))
		{
			recordCurrentLibrary(_library.rootFolder());
			return true;
		}

		QMessageBox::warning(nullptr, tr("Open library"), tr("Could not open the library:\n\n%1\n\nChoose another library folder.").arg(error));
		requestedRoot = QFileDialog::getExistingDirectory(nullptr, tr("Open library"), libraryPickerStartFolder(requestedRoot));
		if (requestedRoot.isEmpty())
			return false;
	}
}

MainWindow::MainWindow(QWidget* parent)
	: QMainWindow(parent)
{
	// All subsequently created objects borrow the library.
	if (!loadInitialLibrary())
		return;

	setWindowTitle("Darkroom");
	resize(1500, 800);
	setAcceptDrops(true);

	_frameViewer = new FrameViewerWindow();

	setupUI();
	_library.setPersistenceFailureHandler([this] { schedulePersistenceFailureWarning(); });

	// restoreSettings() performs the initial grid build after applying the saved filters.
	QMetaObject::invokeMethod(this, [this] {
		restoreSettings();
	}, Qt::QueuedConnection);
}

MainWindow::~MainWindow()
{
	if (!_library.isLoaded())
		return;

	_library.setPersistenceFailureHandler({});
	saveSettings();
	VideoPlayerWindow::closeAll();
	delete _frameViewer;
	delete takeCentralWidget();  // These children borrow _library and must die first.
}

Catalog& MainWindow::libraryCatalog()
{
	return _library.catalog();
}

const Catalog& MainWindow::libraryCatalog() const
{
	return _library.catalog();
}

void MainWindow::setupUI()
{
	auto* central = new QWidget(this);
	setCentralWidget(central);

	auto* rootLayout = new QHBoxLayout(central);
	rootLayout->setContentsMargins(0, 0, 0, 0);
	rootLayout->setSpacing(0);

	_labelSidebar = new LabelSidebar(_library);
	connect(_labelSidebar, &LabelSidebar::filterChanged, this, &MainWindow::refreshMediaGrid);
	connect(_labelSidebar, &LabelSidebar::addLabelRequested, this, &MainWindow::createLabelInteractive);
	connect(_labelSidebar, &LabelSidebar::renameLabelRequested, this, &MainWindow::renameLabelInteractive);
	connect(_labelSidebar, &LabelSidebar::setLabelColorRequested, this, &MainWindow::setLabelColorInteractive);
	connect(_labelSidebar, &LabelSidebar::deleteLabelRequested, this, &MainWindow::deleteLabelInteractive);

	auto* rightPanel = new QWidget();
	auto* mainLayout = new QVBoxLayout(rightPanel);
	mainLayout->setContentsMargins(0, 0, 0, 0);
	mainLayout->setSpacing(0);

	auto* headerWidget = new QWidget();
	auto* headerLayout = new QHBoxLayout(headerWidget);
	headerLayout->setContentsMargins(0, 4, 4, 4);
	headerLayout->setSpacing(4);
	_nameFilter = new QLineEdit();
	_nameFilter->setPlaceholderText(tr("filter by name..."));
	_nameFilter->setClearButtonEnabled(true);
	_nameFilter->setMinimumWidth(220);
	_nameFilter->addAction(Theme::tintedIcon(QStringLiteral(":/UI/icon_search.svg"), &Theme::ThemeColors::MutedText),
		QLineEdit::LeadingPosition);
	connect(_nameFilter, &QLineEdit::textChanged, this, &MainWindow::applyNameFilter);
	headerLayout->addWidget(_nameFilter, 0, Qt::AlignVCenter);

	_mediaTypeFilter = new SegmentedToggle({ tr("All"), tr("Videos"), tr("Photos") });
	_mediaTypeFilter->setCurrentIndex(qBound(0, QSettings{}.value(MEDIA_TYPE_FILTER_KEY, 0).toInt(), 2));
	connect(_mediaTypeFilter, &SegmentedToggle::currentChanged, this, [this](int index) {
		QSettings{}.setValue(MEDIA_TYPE_FILTER_KEY, index);
		refreshMediaGrid();
	});
	headerLayout->addWidget(_mediaTypeFilter, 0, Qt::AlignVCenter);

	auto* focusFilterShortcut = new QShortcut(QKeySequence("Ctrl+F"), this);
	focusFilterShortcut->setContext(Qt::ApplicationShortcut);
	connect(focusFilterShortcut, &QShortcut::activated, this, [this] {
		raise();
		activateWindow();
		_nameFilter->setFocus();
		_nameFilter->selectAll();
	});

	headerLayout->addStretch(1);

	_previewFrameCountCombo = new QComboBox();
	_previewFrameCountCombo->setToolTip(tr("Number of preview frames shown on each video card"));
	const QIcon previewCountIcon = Theme::tintedIcon(QStringLiteral(":/UI/icon_columns.svg"), &Theme::ThemeColors::InstructionText);
	for (int n = MIN_PREVIEW_FRAME_COUNT; n <= MAX_PREVIEW_FRAME_COUNT; ++n)
		_previewFrameCountCombo->addItem(previewCountIcon, (n == 1 ? tr("%1 frame per preview") : tr("%1 frames per preview")).arg(n), n);
	_previewFrameCountCombo->view()->setItemDelegate(new ClosedControlIconDelegate(_previewFrameCountCombo));

	const int savedFrameCount = QSettings{}.value(Settings::PreviewFrameCount, Defaults::PreviewFrameCount).toInt();
	const int savedFrameIdx = _previewFrameCountCombo->findData(savedFrameCount);
	_previewFrameCountCombo->setCurrentIndex(savedFrameIdx >= 0 ? savedFrameIdx
		: _previewFrameCountCombo->findData(Defaults::PreviewFrameCount));

	connect(_previewFrameCountCombo, &QComboBox::currentIndexChanged, this, [this] {
		QSettings{}.setValue(Settings::PreviewFrameCount, _previewFrameCountCombo->currentData().toInt());
		refreshMediaGrid();
	});
	headerLayout->addWidget(_previewFrameCountCombo, 0, Qt::AlignVCenter);

	_sortControl = new SortControl();
	connect(_sortControl, &SortControl::changed, this, &MainWindow::resortMediaGrid);
	headerLayout->addWidget(_sortControl, 0, Qt::AlignVCenter);

	mainLayout->addWidget(headerWidget);

	_mediaGrid = new MediaGrid();
	_mediaGrid->setViewMode(QListView::IconMode);
	_mediaGrid->setFlow(QListView::LeftToRight);
	_mediaGrid->setWrapping(true);
	_mediaGrid->setResizeMode(QListView::Adjust);
	// Video and photo cards have different fixed sizes.
	_mediaGrid->setUniformItemSizes(false);
	_mediaGrid->setMovement(QListView::Static);
	_mediaGrid->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
	_mediaGrid->setSelectionMode(QAbstractItemView::ExtendedSelection);
	_mediaGrid->setDragEnabled(true);
	_mediaGrid->setDragUrlsProvider([this](const QList<QListWidgetItem*>& items) { return dragUrlsForItems(items); });
	_mediaGrid->setSpacing(10);
	Style::applyThemedSheet(_mediaGrid, [] {
		return QStringLiteral("QListWidget::item:selected { background-color: %1; }").arg(Theme::current().AccentBg);
	});

	mainLayout->addWidget(_mediaGrid, 1);

	// Coalesce wheel steps into one grid rebuild.
	_gridZoomDebounce = new QTimer(this);
	_gridZoomDebounce->setSingleShot(true);
	_gridZoomDebounce->setInterval(80);
	connect(_gridZoomDebounce, &QTimer::timeout, this, &MainWindow::refreshMediaGrid);

	auto* splitter = new QSplitter(Qt::Horizontal);
	splitter->addWidget(_labelSidebar);
	splitter->addWidget(rightPanel);
	splitter->setStretchFactor(0, 0);
	splitter->setStretchFactor(1, 1);
	splitter->setCollapsible(0, false);
	rootLayout->addWidget(splitter);

	setupMainMenu();
}

void MainWindow::setupMainMenu()
{
	auto* menuBar = new QMenuBar(this);
	setMenuBar(menuBar);

	QMenu* fileMenu = new QMenu(tr("File"), menuBar);
	fileMenu->addAction(tr("Settings..."), QKeySequence("Ctrl+Alt+P"), this, &MainWindow::openSettings);
	fileMenu->addSeparator();
	fileMenu->addAction(tr("Exit"), QKeySequence("Ctrl+Q"), this, &QMainWindow::close);

	_libraryMenu = new QMenu(tr("Library"), menuBar);
	_libraryMenu->addAction(tr("Open library..."), QKeySequence("Ctrl+O"), this, [this] { pickAndSwitchLibrary(LibraryPickerMode::Open); });
	_libraryMenu->addAction(tr("Create new library..."), this, [this] { pickAndSwitchLibrary(LibraryPickerMode::CreateNew); });
	_libraryMenu->addSeparator();
	connect(_libraryMenu, &QMenu::aboutToShow, this, &MainWindow::rebuildRecentLibraryActions);

	QMenu* editMenu = new QMenu(tr("Edit"), menuBar);
	_deleteAction = editMenu->addAction(tr("Delete"), QKeySequence(Shortcuts::DeleteFile), this, &MainWindow::deleteSelectedItems);
	_removeFromLibraryAction = editMenu->addAction(tr("Remove from library"), QKeySequence(Shortcuts::RemoveFromList), this, &MainWindow::removeSelectedItemsFromLibrary);
	_renameAction = editMenu->addAction(tr("Rename"), QKeySequence(Shortcuts::Rename), this, &MainWindow::renameSelectedItemInteractive);
	connect(_mediaGrid, &QListWidget::itemSelectionChanged, this, &MainWindow::updateEditActions);
	updateEditActions();

	QMenu* toolsMenu = new QMenu(tr("Tools"), menuBar);
	toolsMenu->addAction(tr("Import..."), QKeySequence("Ctrl+Shift+A"), this, [this] { openImportDialog(); });
	toolsMenu->addAction(tr("Scan for untracked files..."), this, &MainWindow::scanForUntrackedFiles);
	toolsMenu->addAction(tr("Check catalog integrity..."), this, &MainWindow::checkCatalogIntegrity);
	toolsMenu->addSeparator();
	toolsMenu->addAction(tr("Compare photos..."), QKeySequence("Shift+C"), this, [this] {
		auto* w = new PhotoCompareWindow({}, this);
		w->setAttribute(Qt::WA_DeleteOnClose);
		w->show();
	});
	toolsMenu->addSeparator();
	toolsMenu->addAction(tr("Restart all videos"), QKeySequence("Shift+R"), this, &VideoPlayerWindow::restartAll);
	toolsMenu->addAction(tr("Close all videos"),   QKeySequence("Shift+W"), this, &VideoPlayerWindow::closeAll);
	toolsMenu->addSeparator();
	toolsMenu->addAction(tr("Re-export all videos"), QKeySequence("Ctrl+Shift+E"), this, &MainWindow::reExportAllVideos);

	QMenu* helpMenu = new QMenu(tr("Help"), menuBar);
	helpMenu->addAction(tr("Show log..."), this, [this] {
		LogViewerDialog(this).exec();
	});
	helpMenu->addSeparator();
	helpMenu->addAction(tr("About Darkroom..."), this, [this] {
		CAboutDialog(QApplication::applicationVersion(), this).exec();
	});

	menuBar->addMenu(fileMenu);
	menuBar->addMenu(_libraryMenu);
	menuBar->addMenu(editMenu);
	menuBar->addMenu(toolsMenu);
	menuBar->addMenu(helpMenu);

	for (QAction* action : menuBar->actions())
	{
		if (!action->menu())
			continue;
		for (QAction* sub : action->menu()->actions())
			sub->setShortcutContext(Qt::ApplicationShortcut);
	}

	// Del/Shift+Del/F2 must not fire while typing in a text field - scope them to the grid.
	for (QAction* a : { _deleteAction, _removeFromLibraryAction, _renameAction })
	{
		_mediaGrid->addAction(a);
		a->setShortcutContext(Qt::WidgetWithChildrenShortcut);
	}
}

void MainWindow::saveSettings()
{
	saveWindowGeometry(this, "mainWindow");
	QVariantList activeIds;
	for (const LabelId id : _labelSidebar->activeLabelIds())
		activeIds << static_cast<qulonglong>(toUInt64(id));
	QSettings{}.setValue("mainWindow/activeLabelIds", activeIds);
	QSettings{}.setValue("mainWindow/labelsAndMode", _labelSidebar->isAndMode());
	QSettings{}.setValue("mainWindow/scrollAnchor", topAnchorKey());
}

void MainWindow::restoreSettings()
{
	restoreWindowGeometry(this, "mainWindow");

	QList<LabelId> activeIds;
	for (const QVariant& v : QSettings{}.value("mainWindow/activeLabelIds").toList())
		activeIds << labelIdFromUInt64(v.toULongLong());
	const bool andMode = QSettings{}.value("mainWindow/labelsAndMode", false).toBool();
	_labelSidebar->setActiveFilter(activeIds, andMode);
	refreshMediaGrid();

	// The wrapped grid's final row layout is available only after post-show resize events.
	const QString scrollAnchorKey = QSettings{}.value("mainWindow/scrollAnchor").toString();
	if (!scrollAnchorKey.isEmpty())
		QMetaObject::invokeMethod(this, [this, scrollAnchorKey] { scrollGridToAnchorKey(scrollAnchorKey); }, Qt::QueuedConnection);
}

bool MainWindow::refuseLibraryChangeWhileProcessing()
{
	if (!_isProcessing)
		return false;

	QMessageBox::information(this, tr("Busy"), tr("The library cannot be changed while media processing is in progress."));
	return true;
}

bool MainWindow::switchLibraryToOrReport(const QString& root, const QString& dialogTitle)
{
	QString error;
	if (switchLibraryTo(root, &error))
		return true;

	QMessageBox::warning(this, dialogTitle, error);
	return false;
}

void MainWindow::pickAndSwitchLibrary(LibraryPickerMode mode)
{
	if (refuseLibraryChangeWhileProcessing())
		return;

	const bool creating = mode == LibraryPickerMode::CreateNew;
	const QString title = creating ? tr("Create new library") : tr("Open library");
	QString startFolder = QFileInfo{ _library.rootFolder() }.absolutePath();
	for(;;)
	{
		const QString requestedRoot = QFileDialog::getExistingDirectory(this, title, libraryPickerStartFolder(startFolder));
		if (requestedRoot.isEmpty())
			return;
		startFolder = requestedRoot;

		if (creating && Library::holdsLibrary(requestedRoot))
		{
			QMessageBox::warning(this, title,
				tr("This folder already holds a library:\n\n%1\n\nUse Open library to open it, or pick another folder for the new one.")
					.arg(QDir::toNativeSeparators(requestedRoot)));
			continue;
		}
		// Treat aliases of the current library as the current library.
		if (!creating)
		{
			const QString requestedReal = QFileInfo(requestedRoot).canonicalFilePath();
			const QString currentReal   = QFileInfo(_library.rootFolder()).canonicalFilePath();
			if (!requestedReal.isEmpty() && requestedReal.compare(currentReal, Qt::CaseInsensitive) == 0)
				return;
		}

		if (switchLibraryToOrReport(requestedRoot, title))
			return;
	}
}

void MainWindow::openRecentLibrary(const QString& root)
{
	if (refuseLibraryChangeWhileProcessing())
		return;

	switchLibraryToOrReport(root, tr("Open library"));
}

void MainWindow::rebuildRecentLibraryActions()
{
	qDeleteAll(_recentLibraryActions);
	_recentLibraryActions.clear();

	const QString currentRoot = _library.rootFolder();
	int number = 0;
	for (const QString& root : recentLibraries())
	{
		QString display = QDir::toNativeSeparators(root);
		display.replace('&', QLatin1String("&&"));

		QAction* action = _libraryMenu->addAction(QString("&%1  %2").arg(++number).arg(display),
			this, [this, root] { openRecentLibrary(root); });

		if (root.compare(currentRoot, Qt::CaseInsensitive) == 0)
		{
			action->setCheckable(true);
			action->setChecked(true);
			action->setEnabled(false);
		}
		_recentLibraryActions.push_back(action);
	}
}

bool MainWindow::switchLibraryTo(const QString& root, QString* error)
{
	if (!_library.setRoot(root, error))
		return false;

	// Finish invalidating users of the old catalog before returning to the event loop.
	VideoPlayerWindow::closeAll();
	_frameViewer->showForFolder({});
	_mediaGrid->clear();
	_contextMenuTarget.reset();
	_labelSidebar->setActiveFilter({}, _labelSidebar->isAndMode());

	recordCurrentLibrary(_library.rootFolder());
	refreshLibraryView();
	return true;
}

void MainWindow::schedulePersistenceFailureWarning()
{
	if (_persistenceWarningQueued)
		return;
	_persistenceWarningQueued = true;
	QMetaObject::invokeMethod(this, [this] {
		_persistenceWarningQueued = false;
		QString error = _library.pendingPersistenceError();
		while (!error.isEmpty())
		{
			QMessageBox message(QMessageBox::Critical, tr("Library save failed"),
				tr("Some library changes are still in memory but could not be saved."),
				QMessageBox::Retry | QMessageBox::Ok, this);
			message.setInformativeText(error);
			message.button(QMessageBox::Ok)->setText(tr("Keep working"));
			if (message.exec() != QMessageBox::Retry)
				return;
			if (_library.flushPendingWrites(&error))
				return;
		}
	}, Qt::QueuedConnection);
}

void MainWindow::openSettings()
{
	if (_isProcessing)
	{
		QMessageBox::information(this, tr("Busy"), tr("Settings cannot be changed while media processing is in progress."));
		return;
	}

	SettingsDialog dialog(this);
	dialog.exec();
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event)
{
	// Reject the grid's exported URLs so they cannot be re-imported into this window.
	if (event->source() == _mediaGrid)
		return;

	if (hasSupportedPaths(event->mimeData()))
		event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* event)
{
	QStringList paths = supportedPaths(event->mimeData());

	// Release the source application's drag before opening the import workflow.
	QMetaObject::invokeMethod(this, [this, paths = std::move(paths)] {
		openImportDialog(paths);
	}, Qt::QueuedConnection);
}

void MainWindow::closeEvent(QCloseEvent* event)
{
	VideoPlayerWindow::closeAll();
	QString error;
	while (!_library.flushPendingWrites(&error))
	{
		QMessageBox message(QMessageBox::Critical, tr("Library save failed"),
			tr("The library still has unsaved changes. Closing now will discard them."),
			QMessageBox::Retry | QMessageBox::Discard | QMessageBox::Cancel, this);
		message.setInformativeText(error);
		message.setDefaultButton(QMessageBox::Cancel);
		const int choice = message.exec();
		if (choice == QMessageBox::Retry)
			continue;
		if (choice != QMessageBox::Discard)
		{
			event->ignore();
			return;
		}
		break;
	}
	QMainWindow::closeEvent(event);
}

// Keep sidebar rebuilding out of refreshMediaGrid(): that function can run from a sidebar item's click signal.
void MainWindow::refreshLibraryView()
{
	refreshMediaGrid();
	_labelSidebar->refresh();
}

void MainWindow::zoomCards(int steps)
{
	const int current = cardImageHeight();
	const int next = qBound(MIN_CARD_IMAGE_HEIGHT, current + steps * CARD_IMAGE_HEIGHT_STEP, MAX_CARD_IMAGE_HEIGHT);
	if (next == current)
		return;

	QSettings{}.setValue(CARD_IMAGE_HEIGHT_KEY, next);
	_gridZoomDebounce->start();
}

namespace {

// Cached so sorting never performs filesystem I/O.
struct ItemInfo {
	bool isBest = false;
	QDateTime date;
	QString name;
};

ItemInfo itemInfoFor(Catalog& catalog, const MediaId& id, bool isBest, bool sortByDate)
{
	const QString name = catalog.displayName(id);
	return { isBest, sortByDate ? getSourceFileDate(catalog.sourcePathForMediaItem(id), catalog.folderForMediaItem(id)) : QDateTime{}, name };
}

// Sort mode is shared by every item in a sort and set immediately before sortItems().
class GridItem final : public QListWidgetItem {
public:
	static int sortBy;
	static bool descending;
	static bool favoritesFirst;

	static void setSortMode(const SortControl* control)
	{
		sortBy = control->sortBy();
		descending = control->descending();
		favoritesFirst = control->favoritesFirst();
	}

	MediaId mediaId;
	ItemInfo info;

	bool operator<(const QListWidgetItem& other) const override {
		const ItemInfo& b = static_cast<const GridItem&>(other).info;
		if (favoritesFirst && info.isBest != b.isBest)
			return info.isBest;
		if (sortBy == SortBy::Date)
			return descending ? b.date < info.date : info.date < b.date;

		const int cmp = QString::compare(info.name, b.name, Qt::CaseInsensitive);
		return descending ? cmp > 0 : cmp < 0;
	}
};
int GridItem::sortBy = SortBy::Name;
bool GridItem::descending = false;
bool GridItem::favoritesFirst = false;

QString gridCaption(int displayNumber, const QString& name)
{
	return QString::number(displayNumber) + ":  " + name;
}

void applyLabelDots(Catalog& catalog, const MediaId& id, MediaItemWidget* card)
{
	std::vector<QColor> dotColors;
	QStringList dotNames;
	for (const LabelId labelId : catalog.labelsForMediaItem(id))
	{
		const Catalog::Label* label = catalog.labelById(labelId);
		if (!label)
			continue;
		dotColors.push_back(label->color.isEmpty() ? QColor() : QColor(label->color));
		dotNames << label->displayName;
	}

	QString stateLine;
	if (catalog.mediaType(id) == Catalog::MediaType::Video)
		stateLine = (catalog.isSplitIntoFrames(id) ? MainWindow::tr("Frames extracted")
		                                           : MainWindow::tr("Not extracted yet - middle-click to extract"))
		            + QLatin1String("\n");
	card->setLabelDots(dotColors, stateLine + MainWindow::tr("Labels: %1").arg(dotNames.join(", ")));
}

void renumberGridCaptions(QListWidget* grid)
{
	int visibleNumber = 0;
	for (int row = 0; row < grid->count(); ++row)
	{
		QListWidgetItem* item = grid->item(row);
		if (item->isHidden())
			continue;
		auto* card = static_cast<MediaItemWidget*>(grid->itemWidget(item));
		card->setLabel(gridCaption(++visibleNumber, static_cast<GridItem*>(item)->info.name));
	}
}

// A leading '^' anchors the otherwise substring-based, case-insensitive match.
bool nameMatchesFilter(const QString& name, const QString& query)
{
	if (query.isEmpty())
		return true;
	if (query.startsWith('^'))
		return name.startsWith(query.mid(1), Qt::CaseInsensitive);
	return name.contains(query, Qt::CaseInsensitive);
}

} // namespace

bool MainWindow::regeneratePreviewFromRealFrames(const QString& folderPath, int frameCount)
{
	QDir folderDir(folderPath);
	const QStringList realFrames = listFrameImageFiles(folderDir);
	if (realFrames.isEmpty())
		return false;

	const QString previewFolder = Catalog::previewDirFor(folderPath);
	if (!QDir{}.mkpath(previewFolder))
		return false;

	// Partial previews are useful, so attempt every copy and succeed if any landed.
	bool anyCopied = false;
	for (const QString& sourceFrame : pickEvenlySpacedFrames(folderDir, realFrames, frameCount))
	{
		if (QFile::copy(sourceFrame, previewFolder + "/" + QFileInfo(sourceFrame).fileName()))
			anyCopied = true;
	}
	return anyCopied;
}

bool MainWindow::regeneratePreviewFor(const MediaId& id)
{
	Catalog& catalog = libraryCatalog();
	const QString folder = catalog.folderForMediaItem(id);
	const int frameCount = _previewFrameCountCombo->currentData().toInt();

	// Real frames are authoritative even if the stored split state disagrees.
	if (regeneratePreviewFromRealFrames(folder, frameCount))
	{
		catalog.markSplitComplete(id);
		return true;
	}

	const QString source = catalog.sourcePathForMediaItem(id);
	if (!QFile::exists(source))
		return false;

	const QString previewDirPath = Catalog::previewDirFor(folder);
	if (!QDir{}.mkpath(previewDirPath))
		assert_and_return_unconditional_r("Failed to create preview folder " + previewDirPath.toStdString(), false);

	const Ffmpeg::PreviewResult result = Ffmpeg::generatePreviewFrames(source, previewDirPath, frameCount);
	catalog.setDurationMs(id, result.durationMs);
	return result.ok();
}

std::vector<MediaId> MainWindow::mediaItemsMatchingFilters() const
{
	const Catalog& catalog = libraryCatalog();

	const QList<LabelId> activeLabelIds = _labelSidebar->activeLabelIds();
	std::vector<MediaId> mediaItems;
	if (activeLabelIds.isEmpty())
	{
		const auto& all = catalog.mediaItems();
		mediaItems.assign(all.keyBegin(), all.keyEnd());
	}
	else
	{
		QSet<MediaId> matched = catalog.mediaItemsForLabel(activeLabelIds.first());
		for (qsizetype i = 1; i < activeLabelIds.size(); ++i)
		{
			const QSet<MediaId> next = catalog.mediaItemsForLabel(activeLabelIds[i]);
			if (_labelSidebar->isAndMode())
				matched.intersect(next);
			else
				matched.unite(next);
		}
		mediaItems.assign(matched.cbegin(), matched.cend());
	}

	if (const int typeFilterIdx = _mediaTypeFilter->currentIndex(); typeFilterIdx != 0)
	{
		const Catalog::MediaType wanted = typeFilterIdx == 2 ? Catalog::MediaType::Photo : Catalog::MediaType::Video;
		std::erase_if(mediaItems, [&](const MediaId& id) { return catalog.mediaType(id) != wanted; });
	}

	return mediaItems;
}

MediaItemWidget* MainWindow::buildMediaCard(const MediaId& id, bool isBest, const QSize& photoCanvas, const QSize& videoCanvas, int previewFrameCount)
{
	Catalog& catalog = libraryCatalog();
	const QString folderPath = catalog.folderForMediaItem(id);
	const bool isPhoto = catalog.mediaType(id) == Catalog::MediaType::Photo;
	QStringList previewPaths;
	if (isPhoto)
	{
		previewPaths << catalog.sourcePathForMediaItem(id);
	}
	else
	{
		// Fall back to full-size frames when the preview cache is missing.
		QDir frameSource(Catalog::previewDirFor(folderPath));
		QStringList imageFiles = listFrameImageFiles(frameSource);
		if (imageFiles.isEmpty())
		{
			frameSource.setPath(folderPath);
			imageFiles = listFrameImageFiles(frameSource);
		}

		previewPaths = pickEvenlySpacedFrames(frameSource, imageFiles, previewFrameCount);
	}

	auto* card = new MediaItemWidget(
		isPhoto ? photoCanvas : videoCanvas,
		previewPaths, QString(),
		id,
		isBest,
		[this, id] { toggleBest(id); },
		[this, id, isPhoto] { if (isPhoto) openSourceInSystemApp(id); else playVideo(id); },
		[this, id](QPoint globalPos) { showMediaItemContextMenu(id, globalPos); },
		/* dynamic size hint */false,
		/* film strip */ !isPhoto
	);
	if (!isPhoto)
	{
		card->setOnMiddleButtonClick([this, id, folderPath] {
			if (ensureFramesSplit(id))
				_frameViewer->showForFolder(folderPath, libraryCatalog().displayName(id));
		});
	}
	card->setOnMouseWheelCallback([this](int steps) { zoomCards(steps); });
	card->setFramesExtracted(!isPhoto && catalog.isSplitIntoFrames(id));
	card->setDuration(catalog.durationMsForMediaItem(id));

	// Defer rebuilding the grid until this card's dropEvent has unwound.
	card->setOnLabelDropped([this, id](const QString& labelId) {
		const std::vector<MediaId> targets = effectiveSelection(id);
		const LabelId dropped = labelIdFromString(labelId);
		const uint64_t libraryGeneration = _library.generation();
		QMetaObject::invokeMethod(this, [this, libraryGeneration, targets, dropped] {
			if (_library.generation() != libraryGeneration)
				return;
			Catalog::BatchScope batch(libraryCatalog());
			for (const MediaId& target : targets)
				libraryCatalog().addLabel(target, dropped);
			refreshLibraryView();
		}, Qt::QueuedConnection);
	});

	applyLabelDots(catalog, id, card);
	return card;
}

void MainWindow::refreshMediaGrid()
{
	const GridViewState viewState = captureGridViewState();
	_mediaGrid->clear();

	const int previewFrameCount = _previewFrameCountCombo->currentData().toInt();
	const int imageHeight = cardImageHeight();

	// Make each video span the same grid width as previewFrameCount photo cards.
	const QSize photoCanvas{ imageHeight, imageHeight };
	const QSize videoCanvas{ MediaItemWidget::videoCanvasWidthForTiling(imageHeight, previewFrameCount, _mediaGrid->spacing()), imageHeight };

	Catalog& catalog = libraryCatalog();
	const QSet<MediaId> bestSet = catalog.mediaItemsForLabel(Catalog::BestLabelId);

	GridItem::setSortMode(_sortControl);
	const bool sortByDate = GridItem::sortBy == SortBy::Date;

	const std::vector<MediaId> mediaItems = mediaItemsMatchingFilters();

	_mediaGrid->setEmptyMessage(catalog.mediaItemCount() == 0
		? tr("The library is empty.\nDrop media files here, or use Tools > Import.")
		: tr("No items match the current filters."));

	// Attaching widgets while inserting makes Qt walk a growing set of persistent indexes: O(N^2).
	std::vector<std::pair<GridItem*, MediaItemWidget*>> pendingAttach;
	pendingAttach.reserve(mediaItems.size());

	// A card's size depends only on its media type.
	QSize videoCardHint, photoCardHint;

	for (const MediaId& id : mediaItems)
	{
		const bool isBest = bestSet.contains(id);
		MediaItemWidget* card = buildMediaCard(id, isBest, photoCanvas, videoCanvas, previewFrameCount);

		auto* item = new GridItem();
		item->mediaId = id;
		item->info = itemInfoFor(catalog, id, isBest, sortByDate);
		QSize& typeHint = catalog.mediaType(id) == Catalog::MediaType::Photo ? photoCardHint : videoCardHint;
		if (!typeHint.isValid())
			typeHint = card->sizeHint();
		item->setSizeHint(typeHint);
		_mediaGrid->addItem(item);

		pendingAttach.emplace_back(item, card);
	}

	_mediaGrid->sortItems(Qt::AscendingOrder);

	for (const auto& [item, card] : pendingAttach)
		_mediaGrid->setItemWidget(item, card);

	applyNameFilter();

	// Restore against the final post-filter layout.
	restoreGridViewState(viewState);
}

QString MainWindow::topAnchorKey() const
{
	for (int row = 0; row < _mediaGrid->count(); ++row)
	{
		const QListWidgetItem* item = _mediaGrid->item(row);
		if (!item->isHidden() && _mediaGrid->visualItemRect(item).bottom() >= 0)
			return static_cast<const GridItem*>(item)->mediaId.key();
	}
	return {};
}

void MainWindow::scrollGridToAnchorKey(const QString& anchorKey)
{
	if (anchorKey.isEmpty())
		return;
	for (int row = 0; row < _mediaGrid->count(); ++row)
	{
		QListWidgetItem* item = _mediaGrid->item(row);
		if (!item->isHidden() && static_cast<const GridItem*>(item)->mediaId.key() == anchorKey)
		{
			_mediaGrid->scrollToItem(item, QAbstractItemView::PositionAtTop);
			return;
		}
	}
}

MainWindow::GridViewState MainWindow::captureGridViewState() const
{
	GridViewState state;
	state.scrollAnchorKey = topAnchorKey();
	for (const QListWidgetItem* item : _mediaGrid->selectedItems())
		state.selectedKeys.insert(static_cast<const GridItem*>(item)->mediaId.key());
	if (const QListWidgetItem* current = _mediaGrid->currentItem())
		state.currentKey = static_cast<const GridItem*>(current)->mediaId.key();
	return state;
}

void MainWindow::restoreGridViewState(const GridViewState& state)
{
	if (!state.selectedKeys.isEmpty() || !state.currentKey.isEmpty())
	{
		QListWidgetItem* currentItem = nullptr;
		{
			const QSignalBlocker blocker{ _mediaGrid };
			for (int row = 0; row < _mediaGrid->count(); ++row)
			{
				auto* item = static_cast<GridItem*>(_mediaGrid->item(row));
				if (item->isHidden())
					continue;
				const QString key = item->mediaId.key();
				if (state.selectedKeys.contains(key))
					item->setSelected(true);
				if (!state.currentKey.isEmpty() && key == state.currentKey)
					currentItem = item;
			}
			if (currentItem)
				_mediaGrid->setCurrentItem(currentItem, QItemSelectionModel::NoUpdate);
		}
		updateEditActions();
	}

	scrollGridToAnchorKey(state.scrollAnchorKey);
}

// Reorder existing cards to avoid re-decoding their thumbnails.
void MainWindow::resortMediaGrid()
{
	const int count = _mediaGrid->count();
	if (count == 0)
		return;

	Catalog& catalog = libraryCatalog();
	const QSet<MediaId> bestSet = catalog.mediaItemsForLabel(Catalog::BestLabelId);

	GridItem::setSortMode(_sortControl);
	const bool sortByDate = GridItem::sortBy == SortBy::Date;

	for (int row = 0; row < count; ++row)
	{
		auto* item = static_cast<GridItem*>(_mediaGrid->item(row));
		item->info = itemInfoFor(catalog, item->mediaId, bestSet.contains(item->mediaId), sortByDate);
	}

	_mediaGrid->sortItems(Qt::AscendingOrder);
	renumberGridCaptions(_mediaGrid);
}

void MainWindow::applyNameFilter()
{
	const QString query = _nameFilter->text().trimmed();
	for (int row = 0; row < _mediaGrid->count(); ++row)
	{
		auto* item = static_cast<GridItem*>(_mediaGrid->item(row));
		const bool hide = !nameMatchesFilter(item->info.name, query);
		item->setHidden(hide);
		if (hide)
			item->setSelected(false);
	}
	renumberGridCaptions(_mediaGrid);
}

std::vector<MediaId> MainWindow::effectiveSelection(std::optional<MediaId> id) const
{
	std::vector<MediaId> selected;
	for (const QListWidgetItem* item : _mediaGrid->selectedItems())
		selected.push_back(static_cast<const GridItem*>(item)->mediaId);
	if (id && std::find(selected.cbegin(), selected.cend(), *id) == selected.cend())
		selected = { *id };
	return selected;
}

QList<QUrl> MainWindow::dragUrlsForItems(const QList<QListWidgetItem*>& items) const
{
	const Catalog& catalog = libraryCatalog();
	QList<QUrl> urls;
	urls.reserve(items.size());
	for (const QListWidgetItem* item : items)
	{
		const QString path = catalog.sourcePathForMediaItem(static_cast<const GridItem*>(item)->mediaId);
		if (!path.isEmpty() && QFile::exists(path))
			urls << QUrl::fromLocalFile(path);
	}
	return urls;
}

QString MainWindow::bulletedItemNameList(const std::vector<MediaId>& selection) const
{
	const Catalog& catalog = libraryCatalog();
	QString list;
	constexpr size_t maxListed = 15;
	for (size_t i = 0; i < std::min(maxListed, selection.size()); ++i)
	{
		const MediaId& sel = selection[i];
		list += "\n• " + (catalog.mediaType(sel) == Catalog::MediaType::Photo
			? sel.name() : catalog.displayName(sel));
	}
	if (selection.size() > maxListed)
		list += "\n" + tr("... and %1 more").arg(selection.size() - maxListed);
	return list;
}

void MainWindow::deleteSelectedItems()
{
	const std::vector<MediaId> selection = effectiveSelection(_contextMenuTarget);
	if (selection.empty())
		return;

	Catalog& catalog = libraryCatalog();

	QString message;
	if (selection.size() == 1)
	{
		const MediaId& sel = selection.front();
		const QString sourcePath = catalog.sourcePathForMediaItem(sel);
		if (catalog.mediaType(sel) == Catalog::MediaType::Photo)
		{
			message = tr("This will permanently delete:\n\n• %1").arg(sourcePath);
		}
		else
		{
			message = tr("This will permanently delete:\n\n• %1").arg(catalog.folderForMediaItem(sel));
			if (!sourcePath.isEmpty())
				message += "\n• " + sourcePath;
		}
	}
	else
	{
		bool anyVideo = false, anyPhoto = false;
		for (const MediaId& sel : selection)
		{
			if (catalog.mediaType(sel) == Catalog::MediaType::Video)
				anyVideo = true;
			else
				anyPhoto = true;
		}

		QStringList deletedKinds;
		if (anyVideo)
			deletedKinds << tr("each video's frame folder and source file");
		if (anyPhoto)
			deletedKinds << tr("each photo's file");
		message = tr("This will permanently delete %1 items - %2:\n").arg(selection.size()).arg(deletedKinds.join(", "));

		message += bulletedItemNameList(selection);
	}
	message += tr("\n\nThis cannot be undone. Continue?");

	if (QMessageBox::warning(this, tr("Delete"), message,
		QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
		return;

	QStringList failedItems;
	{
		// Photo folders are shared by siblings and must never be deleted here.
		Catalog::BatchScope batch(catalog);
		for (const MediaId& sel : selection)
		{
			const QString sourcePath = catalog.sourcePathForMediaItem(sel);
			QStringList failedParts;
			if (catalog.mediaType(sel) == Catalog::MediaType::Photo)
			{
				if (!deleteFileIfPresent(sourcePath))
					failedParts << (sourcePath.isEmpty() ? tr("• Photo file path is missing.") : tr("• Photo file: %1").arg(sourcePath));
			}
			else
			{
				const QString folderPath = catalog.folderForMediaItem(sel);
				const bool folderDeleted = deleteFolderRecursivelyIfPresent(folderPath);
				if (!folderDeleted)
				{
					failedParts << (folderPath.isEmpty() ? tr("• Frame folder path is missing.") : tr("• Frame folder: %1").arg(folderPath));
					if (!sourcePath.isEmpty())
						failedParts << tr("• Source file not attempted: %1").arg(sourcePath);
				}
				else if (!sourcePath.isEmpty() && !deleteFileIfPresent(sourcePath))
					failedParts << tr("• Source file: %1").arg(sourcePath);

				// A failed recursive removal can still have partially changed the folder.
				if (_frameViewer->currentFolder() == folderPath)
					_frameViewer->showForFolder({});
			}

			if (failedParts.empty())
				catalog.removeMediaItem(sel);
			else
				failedItems << tr("%1:\n%2").arg(sel.name(), failedParts.join("\n"));
		}
	}

	refreshLibraryView();

	if (!failedItems.empty())
	{
		MessageBox::notice(this, tr("Delete incomplete"),
			tr("Some items could not be fully deleted. Their catalog records were kept:"),
			failedItems.join("\n\n"), QMessageBox::Critical);
	}
}

void MainWindow::removeSelectedItemsFromLibrary()
{
	const std::vector<MediaId> selection = effectiveSelection(_contextMenuTarget);
	if (selection.empty())
		return;

	Catalog& catalog = libraryCatalog();

	QString message;
	if (selection.size() == 1)
	{
		const MediaId& sel = selection.front();
		message = tr("This will remove the item from the library:\n");
		if (catalog.mediaType(sel) == Catalog::MediaType::Video)
			message += "\n• " + catalog.folderForMediaItem(sel);
		const QString sourcePath = catalog.sourcePathForMediaItem(sel);
		if (!sourcePath.isEmpty())
			message += "\n• " + sourcePath;
	}
	else
	{
		message = tr("This will remove %1 items from the library:\n").arg(selection.size());
		message += bulletedItemNameList(selection);
	}
	message += "\n\n" + tr("No files will be deleted, but labels and other catalog metadata will be discarded. Continue?");

	if (QMessageBox::question(this, tr("Remove from library"), message,
		QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
		return;

	Catalog::BatchScope batch(libraryCatalog());
	for (const MediaId& sel : selection)
		catalog.removeMediaItem(sel);

	refreshLibraryView();
}

void MainWindow::renameSelectedItemInteractive()
{
	const auto selected = _mediaGrid->selectedItems();
	if (selected.size() != 1)
		return;
	renameItemInteractive(static_cast<const GridItem*>(selected.front())->mediaId);
}

void MainWindow::updateEditActions()
{
	const auto selected = _mediaGrid->selectedItems();
	const bool hasSelection = !selected.empty();
	_deleteAction->setEnabled(hasSelection);
	_removeFromLibraryAction->setEnabled(hasSelection);

	_renameAction->setEnabled(selected.size() == 1);
}

void MainWindow::showMediaItemContextMenu(const MediaId& id, const QPoint& globalPos)
{
	_contextMenuTarget = id;
	Catalog& catalog = libraryCatalog();
	const std::vector<MediaId> selection = effectiveSelection(id);
	const QString folderPath = catalog.folderForMediaItem(id);
	const bool isPhoto = catalog.mediaType(id) == Catalog::MediaType::Photo;

	QMenu menu(this);

	const auto addActionMirroringShortcut = [&menu, this](const QString& text, const QAction* shortcutSource, auto&& slot) {
		QAction* a = menu.addAction(text, this, std::forward<decltype(slot)>(slot));
		a->setShortcut(shortcutSource->shortcut());
		a->setShortcutContext(Qt::WidgetShortcut);
	};

	size_t videoCount = 0;
	for (const MediaId& sel : selection)
		if (catalog.mediaType(sel) == Catalog::MediaType::Video)
			++videoCount;
	const bool selectionAllVideos = videoCount == selection.size();
	const bool selectionAllPhotos = videoCount == 0;

	if (selectionAllVideos)
	{
		menu.addAction(selection.size() > 1 ? tr("Compare selected") : tr("Inspect"), [this, selection] {
			QStringList folders;
			for (const MediaId& sel : selection)
				folders << libraryCatalog().folderForMediaItem(sel);
			auto* w = new CompareWindow(folders, this);
			w->setAttribute(Qt::WA_DeleteOnClose);
			w->show();
		});
		menu.addSeparator();
	}

	if (selectionAllPhotos && selection.size() >= 2)
	{
		menu.addAction(tr("Compare photos"), [this, selection] {
			QStringList paths;
			for (const MediaId& sel : selection)
				paths << libraryCatalog().sourcePathForMediaItem(sel);
			PhotoCompareWindow::showForFiles(paths, this);
		});
		menu.addSeparator();
	}

	if (!isPhoto)
	{
		menu.addAction(revealInFileManagerActionText(), [folderPath, this] {
			if (!revealInFileManager(folderPath))
				reportMissingFile(this, folderPath);
		});
	}
	menu.addAction(isPhoto ? tr("Open photo") : tr("Play source video"), [this, id] {
		openSourceInSystemApp(id);
	});
	menu.addAction(tr("Locate source file"), [this, id] {
		const QString sourcePath = libraryCatalog().sourcePathForMediaItem(id);
		if (sourcePath.isEmpty())
			QMessageBox::warning(this, tr("Error"), tr("No source file is recorded for this item."));
		else if (!revealInFileManager(sourcePath))
			reportMissingFile(this, sourcePath);
	});
	menu.addAction(tr("Copy source path to clipboard"), [this, id] {
		const QString sourcePath = libraryCatalog().sourcePathForMediaItem(id);
		if (!sourcePath.isEmpty())
			QApplication::clipboard()->setText(QDir::toNativeSeparators(sourcePath));
	});
	addActionMirroringShortcut(isPhoto ? tr("Rename photo") : tr("Rename media file"), _renameAction,
		[this, id] { renameItemInteractive(id); });
	menu.addSeparator();

	const bool inBest = isInBest(id);
	menu.addAction(inBest ? tr("Remove from Best") : tr("Add to Best"), [this, id] {
		toggleBest(id);
	});

	std::vector<LabelVisuals::ChecklistRow> labelRows;
	for (const Catalog::Label& label : catalog.allLabels())
	{
		if (label.isVirtual())
			continue;
		const LabelId labelId = label.id;

		int haveCount = 0;
		for (const MediaId& sel : selection)
			if (catalog.mediaItemHasLabel(sel, labelId))
				++haveCount;

		labelRows.push_back({ label.displayName, QColor(label.color),
			LabelVisuals::presenceForCount(haveCount, static_cast<int>(selection.size())),
			[this, selection, labelId](bool addToAll) {
				Catalog::BatchScope batch(libraryCatalog());
				for (const MediaId& target : selection)
				{
					if (addToAll)
						libraryCatalog().addLabel(target, labelId);
					else
						libraryCatalog().removeLabel(target, labelId);
				}
				refreshLibraryView();
			} });
	}
	LabelVisuals::buildChecklistMenu(menu.addMenu(tr("Labels")), std::move(labelRows));
	menu.addSeparator();

	addActionMirroringShortcut(selection.size() > 1 ? tr("Remove %1 items from library (untrack)").arg(selection.size()) : tr("Remove from library (untrack)"),
		_removeFromLibraryAction, &MainWindow::removeSelectedItemsFromLibrary);
	addActionMirroringShortcut(selection.size() > 1 ? tr("Delete (%1 items)").arg(selection.size()) : tr("Delete"),
		_deleteAction, &MainWindow::deleteSelectedItems);

	menu.exec(globalPos);
	_contextMenuTarget = std::nullopt;
}

void MainWindow::playVideo(const MediaId& id)
{
	const QString sourcePath = libraryCatalog().sourcePathForMediaItem(id);
	if (!QFile::exists(sourcePath))
	{
		reportMissingFile(this, sourcePath);
		return;
	}

	auto* playerWindow = new VideoPlayerWindow(_library, sourcePath, id, nullptr);
	playerWindow->show();
}

void MainWindow::openSourceInSystemApp(const MediaId& id)
{
	const QString sourcePath = libraryCatalog().sourcePathForMediaItem(id);
	if (!QFile::exists(sourcePath))
	{
		reportMissingFile(this, sourcePath);
		return;
	}

	QDesktopServices::openUrl(QUrl::fromLocalFile(sourcePath));
}

bool MainWindow::resplitVideoIntoFrames(const MediaId& id, bool preserveExistingPreview)
{
	Catalog& catalog = libraryCatalog();
	assert_and_return_r(catalog.containsMediaItem(id), false);
	const QString videoFilePath = catalog.sourcePathForMediaItem(id);
	const QString outputFolder = catalog.folderForMediaItem(id);
	assert_and_return_r(!outputFolder.isEmpty(), false);  // QDir("") addresses the working directory
	const bool hadExistingFolder = QDir(outputFolder).exists();
	const QString preservedFolder = hadExistingFolder
		? QFileInfo(outputFolder).dir().filePath(".darkroom-resplit-" + QUuid::createUuid().toString(QUuid::Id128))
		: QString{};

	if (hadExistingFolder && !QDir{}.rename(outputFolder, preservedFolder))
	{
		QMessageBox::critical(this, tr("Error"), tr("Failed to preserve the existing frame folder before replacing it:\n%1").arg(outputFolder));
		return false;
	}

	// Until commit, every exit restores the previous folder with same-filesystem renames.
	auto rollback = qScopeGuard([&] {
		if (!deleteFolderRecursivelyIfPresent(outputFolder))
		{
			QString message = tr("Failed to discard the replacement frame folder:\n%1").arg(outputFolder);
			if (hadExistingFolder)
				message += "\n\n" + tr("The previous frame folder remains preserved at:\n%1").arg(preservedFolder);
			QMessageBox::critical(this, tr("Error"), message);
			return;
		}

		if (hadExistingFolder && !QDir{}.rename(preservedFolder, outputFolder))
		{
			QMessageBox::critical(this, tr("Error"),
				tr("Failed to restore the previous frame folder.\n\nPreserved folder:\n%1\n\nOriginal location:\n%2")
					.arg(preservedFolder).arg(outputFolder));
		}
	});

	if (!splitVideoIntoFrames(videoFilePath, outputFolder))
		return false;

	const QString preservedPreviewDir = Catalog::previewDirFor(preservedFolder);
	const bool hasPreviewToPreserve = preserveExistingPreview && hadExistingFolder && QDir(preservedPreviewDir).exists();
	if (hasPreviewToPreserve)
	{
		if (!QDir{}.rename(preservedPreviewDir, Catalog::previewDirFor(outputFolder)))
		{
			QMessageBox::critical(this, tr("Error"),
				tr("Failed to carry the existing preview into the new frame folder.\n\nPreserved folder:\n%1\n\nNew folder:\n%2")
					.arg(preservedFolder).arg(outputFolder));
			return false;
		}
	}
	else
	{
		const Ffmpeg::PreviewResult result = Ffmpeg::generatePreviewFrames(videoFilePath, Catalog::previewDirFor(outputFolder), _previewFrameCountCombo->currentData().toInt());
		catalog.setDurationMs(id, result.durationMs);
	}

	rollback.dismiss();
	catalog.markSplitComplete(id);

	if (hadExistingFolder && !deleteFolderRecursivelyIfPresent(preservedFolder))
	{
		QMessageBox::warning(this, tr("Cleanup incomplete"),
			tr("The new frames are ready, but the previous frame folder could not be completely removed:\n%1").arg(preservedFolder));
	}
	return true;
}

bool MainWindow::ensureFramesSplit(const MediaId& id)
{
	Catalog& catalog = libraryCatalog();
	if (catalog.isSplitIntoFrames(id))
		return true;

	return resplitVideoIntoFrames(id, /*preserveExistingPreview=*/true);
}

bool MainWindow::splitVideoIntoFrames(const QString& videoFilePath, const QString& outputFolder)
{
	const Ffmpeg::SplitOptions options{ .tiff = useTiff(), .jpegQuality = jpegQuality(), .frameStep = frameStep() };
	const Ffmpeg::SplitResult result = Ffmpeg::splitVideoIntoFrames(videoFilePath, outputFolder, options);
	if (!result.ok())
	{
		reportFfmpegFailure(this, result, videoFilePath, outputFolder);
		return false;
	}
	return true;
}

void MainWindow::importVideoBatch(QStringList videoPaths, const QString& storageFolderPath, const QHash<MediaId, QString>& stagedPreviewDirs, const QHash<MediaId, qint64>& stagedDurations)
{
	if (videoPaths.empty())
		return;

	if (_isProcessing)
	{
		QMessageBox::information(this, tr("Busy"), tr("Already extracting frames. Please wait for the current operation to finish."));
		return;
	}
	_isProcessing = true;
	const auto processingGuard = qScopeGuard([this] { _isProcessing = false; });

	Catalog::BatchScope batch(libraryCatalog());

	QMessageBox progressBox(this);
	progressBox.setWindowTitle(tr("Processing"));
	progressBox.setStandardButtons(QMessageBox::NoButton);
	progressBox.setModal(true);
	progressBox.show();

	const auto partition = std::ranges::stable_partition(videoPaths, [&storageFolderPath](const QString& path) {
		const QString outputFolder = storageFolderPath + "/" + QFileInfo(path).completeBaseName();
		return !QDir{ outputFolder }.exists();
	});

	const auto processFilesRange = [&progressBox, this, &storageFolderPath, &stagedPreviewDirs, &stagedDurations, totalSize = videoPaths.size()](const auto& begin, const auto& end, qsizetype firstNumber, bool overwriteExisting = false) {
		qsizetype displayNumber = firstNumber;
		for (const QString& videoPath : std::ranges::subrange(begin, end))
		{
			progressBox.setText(tr("Adding video %1/%2...").arg(displayNumber++).arg(totalSize));
			QApplication::processEvents();
			const MediaId id = MediaId::fromFile(videoPath);
			const QString stagedPreviewDir = stagedPreviewDirs.value(id);
			const qint64 stagedDurationMs = stagedDurations.value(id, -1);
			Import::Result result = Import::importVideo(libraryCatalog(), videoPath, storageFolderPath, stagedPreviewDir, overwriteExisting, stagedDurationMs);
			if (result.status == Import::Status::FolderConflict)
			{
				const QString outputFolder = storageFolderPath + "/" + QFileInfo(videoPath).completeBaseName();
				if (QMessageBox::question(this, tr("Folder Exists"),
						tr("Folder already exists:\n%1\n\nOverwrite?").arg(outputFolder),
						QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
					continue;
				result = Import::importVideo(libraryCatalog(), videoPath, storageFolderPath, stagedPreviewDir, /*overwriteExisting=*/true, stagedDurationMs);
			}
			if (result.status == Import::Status::Error)
				QMessageBox::critical(this, tr("Error"), result.errorMessage);
		}
	};

	processFilesRange(videoPaths.begin(), partition.begin(), 1);

	if (partition.begin() != partition.end())
	{
		QMessageBox msgBox;
		msgBox.setIcon(QMessageBox::Question);
		msgBox.setWindowTitle(tr("Folder conflict"));
		msgBox.setText(tr("One or more videos have existing output folders. Overwrite all, skip all, or decide one by one?"));
		msgBox.setStandardButtons(QMessageBox::YesToAll | QMessageBox::Yes | QMessageBox::NoToAll);
		msgBox.button(QMessageBox::YesToAll)->setText(tr("Overwrite all"));
		msgBox.button(QMessageBox::Yes)->setText(tr("Decide one by one"));
		msgBox.button(QMessageBox::NoToAll)->setText(tr("Skip all"));
		msgBox.setDefaultButton(QMessageBox::YesToAll);

		const auto choice = msgBox.exec();
		if (choice != QMessageBox::NoToAll)
		{
			processFilesRange(partition.begin(), partition.end(), partition.begin() - videoPaths.begin() + 1, choice == QMessageBox::YesToAll);
		}
	}

	refreshLibraryView();
}

std::vector<Import::PhotoResult> MainWindow::importPhotoBatch(LabelId labelId, const QStringList& photoPaths, Import::PhotoImportMode mode)
{
	Catalog& catalog = libraryCatalog();
	const Catalog::Label* label = catalog.labelById(labelId);
	if (!label || label->isVirtual())
		return {};
	const QString photoFolder = catalog.photoFolderForLabel(labelId);
	if (photoFolder.isEmpty())
	{
		QMessageBox::warning(this, tr("Import"),
			tr("This label does not have a safe photo-storage path:\n%1").arg(label->displayName));
		return {};
	}

	Catalog::BatchScope batch(catalog);

	std::vector<Import::PhotoResult> results;
	results.reserve(photoPaths.size());
	for (const QString& path : photoPaths)
	{
		const Import::PhotoResult result = Import::importPhoto(catalog, photoFolder, path, mode);
		if (result.status == Import::PhotoStatus::Error)
			QMessageBox::critical(this, tr("Error"), result.errorMessage);
		// Referenced photos have no storage folder from which to derive this label.
		if (result.status == Import::PhotoStatus::Success && mode == Import::PhotoImportMode::Reference)
			catalog.addLabel(result.registeredId, labelId);
		results.push_back(result);
	}

	refreshLibraryView();
	return results;
}

void MainWindow::reExportAllVideos()
{
	if (_isProcessing)
	{
		QMessageBox::information(this, tr("Busy"), tr("Already extracting frames. Please wait for the current operation to finish."));
		return;
	}

	const auto confirm = QMessageBox::question(this, tr("Re-export all videos"),
		tr("This will delete and re-export all video frame folders where the source video is still available.\n\n"
		   "This applies to all videos in the library.\n\nContinue?"),
		QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
	if (confirm != QMessageBox::Yes)
		return;

	std::vector<MediaId> toReExport;
	Catalog& catalog = libraryCatalog();
	for (const auto& [id, entry] : catalog.mediaItems().asKeyValueRange())
	{
		if (entry.type != Catalog::MediaType::Video)
			continue;

		if (!entry.sourcePath.isEmpty() && QFile::exists(entry.sourcePath))
			toReExport.push_back(id);
	}

	if (toReExport.empty())
	{
		QMessageBox::information(this, tr("Re-export all videos"), tr("No folders with an available source video were found."));
		return;
	}

	_isProcessing = true;
	const auto processingGuard = qScopeGuard([this] { _isProcessing = false; });

	Catalog::BatchScope batch(catalog);

	QMessageBox progressBox(this);
	progressBox.setWindowTitle(tr("Re-exporting"));
	progressBox.setStandardButtons(QMessageBox::NoButton);
	progressBox.setModal(true);
	progressBox.show();

	for (size_t i = 0, total = toReExport.size(); i < total; ++i)
	{
		progressBox.setText(tr("Re-exporting video %1/%2...").arg(i + 1).arg(total));
		QApplication::processEvents();

		static_cast<void>(resplitVideoIntoFrames(toReExport[i], /*preserveExistingPreview=*/false));
	}

	refreshMediaGrid();
}

LabelId MainWindow::createFolderLabel(const QString& name, const QString& color, bool refreshList)
{
	QString error;
	const LabelId labelId = libraryCatalog().createLabel(name, color, &error);
	if (labelId == LabelId::None)
	{
		QMessageBox::warning(this, tr("Create label"), error);
		return {};
	}

	if (refreshList)
		refreshLibraryView();
	return labelId;
}

void MainWindow::createLabelInteractive()
{
	bool ok = false;
	const QString name = QInputDialog::getText(this, tr("New label"), tr("Label name:"), QLineEdit::Normal, QString{}, &ok);
	if (ok)
		createFolderLabel(name);
}

void MainWindow::renameLabelInteractive(LabelId labelId)
{
	const Catalog::Label* label = libraryCatalog().labelById(labelId);
	if (!label)
		return;

	bool ok = false;
	const QString newName = QInputDialog::getText(this, tr("Rename label"), tr("New name:"), QLineEdit::Normal, label->displayName, &ok);
	if (!ok || newName == label->displayName)
		return;

	QString error;
	if (!libraryCatalog().renameLabel(labelId, newName, &error))
	{
		QMessageBox::warning(this, tr("Rename label"), error);
		return;
	}
	refreshLibraryView();
}

void MainWindow::setLabelColorInteractive(LabelId labelId)
{
	const Catalog::Label* label = libraryCatalog().labelById(labelId);
	if (!label)
		return;

	const QColor initial = label->color.isEmpty() ? QColor(Qt::white) : QColor(label->color);
	const QColor chosen = QColorDialog::getColor(initial, this, tr("Label color"));
	if (!chosen.isValid())
		return;

	libraryCatalog().setColor(labelId, chosen.name());
	refreshLibraryView();
}

void MainWindow::deleteLabelInteractive(LabelId labelId)
{
	Catalog& catalog = libraryCatalog();
	const Catalog::Label* label = catalog.labelById(labelId);
	if (!label)
		return;
	const QString name = label->displayName;

	const Catalog::DeleteImpact impact = catalog.deleteLabelImpact(labelId);
	if (impact.wouldOrphan)
	{
		QMessageBox::warning(this, tr("Delete label"),
			tr("Cannot delete \"%1\": some items are stored only under this label, with no other label to "
			   "fall back on. Give those items another label first, then delete this one.").arg(name));
		return;
	}

	QString message = tr("Delete the label \"%1\"?").arg(name);
	if (impact.relocateCount > 0)
		message += tr("\n\n%1 item(s) stored under it will be moved to another of their labels.").arg(impact.relocateCount);
	if (impact.untagCount > 0)
		message += tr("\n%1 item(s) tagged with it will lose the tag.").arg(impact.untagCount);
	message += tr("\n\nThis cannot be undone. Continue?");

	if (QMessageBox::warning(this, tr("Delete label"), message, QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
		return;

	if (!catalog.deleteLabel(labelId))
		QMessageBox::warning(this, tr("Delete label"), tr("Could not fully delete \"%1\" - some items may not have been moved.").arg(name));
	refreshLibraryView();
}

void MainWindow::openImportDialog(const QStringList& initialStaging)
{
	ImportDialog::Callbacks callbacks{
		.addMediaItemsRequested = [this](const QString& labelId, const QStringList& videoPaths,
				const QHash<MediaId, QString>& stagedPreviewDirs, const QHash<MediaId, qint64>& stagedDurations) {
			const LabelId id = labelIdFromString(labelId);
			const QString storageFolder = libraryCatalog().storageFolderForLabel(id);
			if (storageFolder.isEmpty())
			{
				const Catalog::Label* label = libraryCatalog().labelById(id);
				QMessageBox::warning(this, tr("Import"),
					tr("This label does not have a safe storage path:\n%1").arg(label ? label->displayName : labelId));
				return;
			}
			importVideoBatch(videoPaths, storageFolder, stagedPreviewDirs, stagedDurations);
		},
		.importPhotosRequested = [this](const QString& labelId, const QStringList& photoPaths, Import::PhotoImportMode mode) {
			return importPhotoBatch(labelIdFromString(labelId), photoPaths, mode);
		},
		.createLabelRequested = [this](const QString& name, const QString& color) -> QString {
			const LabelId id = createFolderLabel(name, color, /*refreshList*/ false);
			return id == LabelId::None ? QString{} : toString(id);
		},
		.viewChanged = [this] { refreshLibraryView(); }
	};

	ImportDialog dialog(_library, std::move(callbacks), libraryCatalog().anySourceDir(), nullptr);
	if (!initialStaging.isEmpty())
		dialog.addToStaging(initialStaging);
	dialog.exec();

	// A label created without a subsequent import has not reached viewChanged.
	refreshLibraryView();
}

void MainWindow::scanForUntrackedFiles()
{
	QSet<QString> tracked;
	Catalog& catalog = libraryCatalog();
	for (const Catalog::Entry& entry : catalog.mediaItems())
	{
		if (!entry.sourcePath.isEmpty())
			tracked.insert(pathComparisonKey(entry.sourcePath));
	}

	QSettings settings;
	constexpr const char* lastFolderKey = "untrackedScan/lastFolder";
	const QString defaultStartDir = tracked.empty() ? _library.rootFolder() : QFileInfo(*tracked.begin()).absolutePath();
	QString startDir = settings.value(lastFolderKey, defaultStartDir).toString();
	if (!QFileInfo(startDir).exists())
		startDir = defaultStartDir;
	const QString dir = QFileDialog::getExistingDirectory(this, tr("Scan folder for untracked media"), startDir);
	if (dir.isEmpty())
		return;
	settings.setValue(lastFolderKey, dir);

	const std::optional<int> depth = MessageBox::question(this, tr("Scan for untracked media"),
		tr("Scan subfolders as well, or only this folder?\n\n%1").arg(QDir::toNativeSeparators(dir)),
		{tr("Include Subfolders"), tr("This Folder Only")});
	if (!depth)
		return;
	const bool scanRecursively = *depth == 0;

	// Match both the selected spelling and its canonical spelling when scanning through an alias.
	const QString realScanRoot = QFileInfo(dir).canonicalFilePath();
	const QDir scanRoot(dir);
	const bool aliasedScanRoot = !realScanRoot.isEmpty() && pathComparisonKey(realScanRoot) != pathComparisonKey(dir);

	QStringList untracked;
	for (const QString& path : collectFilesInDirectory(dir, scanRecursively, isSupportedMediaFile))
	{
		const bool isTracked = tracked.contains(pathComparisonKey(path))
			|| (aliasedScanRoot && tracked.contains(pathComparisonKey(realScanRoot + '/' + scanRoot.relativeFilePath(path))));
		if (!isTracked)
			untracked.push_back(QDir::toNativeSeparators(path));
	}

	if (untracked.isEmpty())
	{
		const QString message = scanRecursively
			? tr("No untracked media files were found under:\n%1").arg(QDir::toNativeSeparators(dir))
			: tr("No untracked media files were found directly in:\n%1").arg(QDir::toNativeSeparators(dir));
		QMessageBox::information(this, tr("Scan complete"), message);
		return;
	}

	std::ranges::sort(untracked, &NaturalSort::lessCaseInsensitive);
	openImportDialog(untracked);
}

void MainWindow::checkCatalogIntegrity()
{
	IntegrityCheckDialog::Callbacks callbacks{
		.registerRequested = [this](const QString& folderPath, const QString& sourcePath) {
			return libraryCatalog().addMediaItem(MediaId::fromFile(sourcePath), sourcePath, folderPath, /*splitIntoFrames=*/true);
		},
		.adoptPhotoRequested = [this](const QString& filePath) {
			Catalog& catalog = libraryCatalog();
			const QString labelDir = QFileInfo(filePath).absolutePath();
			return catalog.addPhoto(MediaId::fromFile(filePath), filePath, labelDir, /*referenced=*/false);
		},
		.reimportRequested = [this](const MediaId& id) {
			return resplitVideoIntoFrames(id, /*preserveExistingPreview=*/false);
		},
		.regeneratePreviewRequested = [this](const MediaId& id) {
			return regeneratePreviewFor(id);
		},
		.markSplitRequested = [this](const MediaId& id) {
			libraryCatalog().markSplitComplete(id);
			return true;
		},
		.locateSourceRequested = [this](const MediaId& id, const QString& newSourcePath) {
			Catalog& catalog = libraryCatalog();
			return catalog.applyRename(id, MediaId::fromFile(newSourcePath), newSourcePath, catalog.folderForMediaItem(id));
		},
		.removeEntryRequested = [this](const MediaId& id) {
			libraryCatalog().removeMediaItem(id);
			return true;
		},
		.locatePhotoRequested = [this](const MediaId& id, const QString& newSourcePath) {
			return libraryCatalog().applyRename(id, MediaId::fromFile(newSourcePath), newSourcePath, /*newFolderAbs=*/QString{});
		},
	};

	if (IntegrityCheckDialog::scanAndShowUi(libraryCatalog(), _library.rootFolder(), std::move(callbacks), this))
		refreshLibraryView();
}

bool MainWindow::isInBest(const MediaId& id) const
{
	return libraryCatalog().mediaItemHasLabel(id, Catalog::BestLabelId);
}

void MainWindow::toggleBest(const MediaId& id)
{
	Catalog& catalog = libraryCatalog();
	if (catalog.mediaItemHasLabel(id, Catalog::BestLabelId))
		catalog.removeLabel(id, Catalog::BestLabelId);
	else
		catalog.addLabel(id, Catalog::BestLabelId);

	// Toggling the star does not recreate the card's label dots.
	for (int row = 0; row < _mediaGrid->count(); ++row)
	{
		auto* item = static_cast<GridItem*>(_mediaGrid->item(row));
		if (item->mediaId == id)
		{
			applyLabelDots(catalog, id, static_cast<MediaItemWidget*>(_mediaGrid->itemWidget(item)));
			break;
		}
	}

	if (_labelSidebar->activeLabelIds().contains(Catalog::BestLabelId))
		QMetaObject::invokeMethod(this, &MainWindow::refreshMediaGrid, Qt::QueuedConnection);
	else if (_sortControl->favoritesFirst())
		QMetaObject::invokeMethod(this, &MainWindow::resortMediaGrid, Qt::QueuedConnection);
}

void MainWindow::renameItemInteractive(const MediaId& id)
{
	const MediaRename::Result result = MediaRename::renameItemInteractive(libraryCatalog(), id, this);
	if (!result.renamed)
		return;

	refreshLibraryView();
	if (!result.oldFolderPath.isEmpty() && _frameViewer->currentFolder() == result.oldFolderPath && _frameViewer->isVisible())
		_frameViewer->showForFolder(result.newFolderPath, result.newName);
}
