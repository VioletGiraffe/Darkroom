#include "Windows/MainWindow.h"
#include "Core/Catalog.h"
#include "Core/Library.h"
#include "Windows/PhotoCompareWindow.h"
#include "Windows/FrameExtraction.h"
#include "Windows/FrameViewerWindow.h"
#include "Windows/IntegrityCheckDialog.h"
#include "UiComponents/MediaBrowserWidget.h"
#include "Windows/ImportDialog.h"
#include "Windows/LogViewerDialog.h"
#include "Windows/SettingsDialog.h"
#include "Core/MediaId.h"
#include "Windows/VideoPlayerWindow.h"
#include "Utils.h"
#include "Settings.h"
#include "Shortcuts.h"

#include "aboutdialog/caboutdialog.h"
#include "dialogs/messagebox.h"
#include "utils/naturalsorting/cnaturalsorterqcollator.h"

#include <QAbstractButton>
#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>

#include <QScopeGuard>

#include <algorithm>
#include <utility>
#include <vector>

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
	_mediaBrowser = new MediaBrowserWidget(_library, this);
	setCentralWidget(_mediaBrowser);

	connect(_mediaBrowser, &MediaBrowserWidget::inspectVideoFramesRequested, this, [this](const MediaId& id) {
		if (FrameExtraction::ensureFramesExtracted(
				libraryCatalog(), id, _mediaBrowser->previewFrameCount(), this))
			_frameViewer->showForFolder(libraryCatalog().folderForMediaItem(id), libraryCatalog().displayName(id));
	});
	connect(_mediaBrowser, &MediaBrowserWidget::frameFolderPathChanged, this,
		[this](const QString& oldFolderPath, const QString& newFolderPath, const QString& newDisplayName) {
			if (_frameViewer->currentFolder() != oldFolderPath)
				return;
			if (newFolderPath.isEmpty())
				_frameViewer->showForFolder({});
			else if (_frameViewer->isVisible())
				_frameViewer->showForFolder(newFolderPath, newDisplayName);
		});

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
	_deleteAction = editMenu->addAction(
		tr("Delete"), QKeySequence(Shortcuts::DeleteFile), _mediaBrowser, &MediaBrowserWidget::deleteSelectedMediaItemsInteractive);
	_removeFromLibraryAction = editMenu->addAction(tr("Remove from library"), QKeySequence(Shortcuts::RemoveFromList),
		_mediaBrowser, &MediaBrowserWidget::removeSelectedMediaItemsFromLibraryInteractive);
	_renameAction = editMenu->addAction(
		tr("Rename"), QKeySequence(Shortcuts::Rename), _mediaBrowser, &MediaBrowserWidget::renameSelectedMediaItemInteractive);
	connect(_mediaBrowser, &MediaBrowserWidget::selectionChanged, this, &MainWindow::updateEditActions);
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
		_mediaBrowser->installGridAction(a);
		a->setShortcutContext(Qt::WidgetWithChildrenShortcut);
	}
}

void MainWindow::saveSettings()
{
	saveWindowGeometry(this, "mainWindow");
	_mediaBrowser->saveSettings();
}

void MainWindow::restoreSettings()
{
	restoreWindowGeometry(this, "mainWindow");
	_mediaBrowser->restoreSettings();
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
	_mediaBrowser->resetForLibrarySwitch();

	recordCurrentLibrary(_library.rootFolder());
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
	if (_mediaBrowser->isGridDragSource(event->source()))
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

void MainWindow::updateEditActions()
{
	const std::vector<MediaId> selected = _mediaBrowser->selectedMediaItems();
	const bool hasSelection = !selected.empty();
	_deleteAction->setEnabled(hasSelection);
	_removeFromLibraryAction->setEnabled(hasSelection);

	_renameAction->setEnabled(selected.size() == 1);
}

void MainWindow::reExportAllVideos()
{
	if (_isProcessing)
	{
		QMessageBox::information(this, tr("Busy"), tr("Already extracting frames. Please wait for the current operation to finish."));
		return;
	}

	_isProcessing = true;
	const auto processingGuard = qScopeGuard([this] { _isProcessing = false; });

	if (FrameExtraction::reExportAllVideosInteractive(
			libraryCatalog(), _mediaBrowser->previewFrameCount(), this))
		_mediaBrowser->refreshLibraryView();
}

void MainWindow::openImportDialog(const QStringList& initialStaging)
{
	ImportDialog dialog(_library, libraryCatalog().anySourceDir(), nullptr);
	if (!initialStaging.isEmpty())
		dialog.addToStaging(initialStaging);
	dialog.exec();
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
			return FrameExtraction::reextractVideoFrames(libraryCatalog(), id,
				FrameExtraction::PreviewHandling::Regenerate, _mediaBrowser->previewFrameCount(), this);
		},
		.regeneratePreviewRequested = [this](const MediaId& id) {
			return FrameExtraction::regeneratePreview(libraryCatalog(), id, _mediaBrowser->previewFrameCount());
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
		.removeInvalidLabelReferencesRequested = [this](const MediaId& id) {
			return libraryCatalog().removeInvalidLabelReferences(id);
		},
	};

	bool catalogOrStorageChanged = false;
	{
		// This spans dialog.exec()'s nested event loop; a BatchScope would retain a persistence writer across re-entrant UI processing.
		Catalog::ChangeBatchScope catalogChanges(libraryCatalog());
		catalogOrStorageChanged = IntegrityCheckDialog::scanAndShowUi(libraryCatalog(), _library.rootFolder(), std::move(callbacks), this);
	}
	if (catalogOrStorageChanged)
		_mediaBrowser->refreshLibraryView();
}
