#pragma once

#include "Core/Library.h"
#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QMainWindow>
#include <QStringList>
RESTORE_COMPILER_WARNINGS

#include <vector>

class Catalog;
class FrameViewerWindow;
class MediaBrowserWidget;
class QAction;
class QMenu;
class QWidget;

class MainWindow final : public QMainWindow
{
public:
	// Loads before constructing borrowers. Cancelling leaves the window unbuilt; main() must check isLibraryLoaded().
	explicit MainWindow(QWidget* parent = nullptr);
	~MainWindow();

	[[nodiscard]] bool isLibraryLoaded() const { return _library.isLoaded(); }

protected:
	void dragEnterEvent(QDragEnterEvent* event) override;
	void dropEvent(QDropEvent* event) override;
	void closeEvent(QCloseEvent* event) override;

private:
	[[nodiscard]] bool loadInitialLibrary();

	void setupUI();
	void setupMainMenu();

	void saveSettings();
	void restoreSettings();

	void reExportAllVideos();
	void openImportDialog(const QStringList& initialStaging = {});
	void scanForUntrackedFiles();
	void checkCatalogIntegrity();

	void updateEditActions();

	enum class LibraryPickerMode { Open, CreateNew };
	void pickAndSwitchLibrary(LibraryPickerMode mode);
	void openRecentLibrary(const QString& root);
	[[nodiscard]] bool switchLibraryTo(const QString& root, QString* error);
	bool switchLibraryToOrReport(const QString& root, const QString& dialogTitle);
	// Event-pumping work must not outlive its catalog/store borrows.
	[[nodiscard]] bool refuseLibraryChangeWhileProcessing();
	void rebuildRecentLibraryActions();
	void schedulePersistenceFailureWarning();
	void openSettings();

private:
	[[nodiscard]] Catalog& libraryCatalog();
	[[nodiscard]] const Catalog& libraryCatalog() const;

	// Declared first so it outlives the other C++ members; the destructor explicitly deletes Qt children
	// that borrow it before member destruction begins.
	Library _library;
	MediaBrowserWidget* _mediaBrowser = nullptr;
	FrameViewerWindow* _frameViewer = nullptr;

	QAction* _deleteAction = nullptr;
	QAction* _removeFromLibraryAction = nullptr;
	QAction* _renameAction = nullptr;

	QMenu* _libraryMenu = nullptr;
	std::vector<QAction*> _recentLibraryActions;

	bool _isProcessing = false;
	bool _persistenceWarningQueued = false;
};
