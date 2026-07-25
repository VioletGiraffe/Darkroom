#pragma once

#include "Core/Library.h"
#include "Core/LabelId.h"
#include "Core/MediaId.h"
#include "Import.h"

#include <QHash>
#include <QMainWindow>
#include <QStringList>

#include <optional>
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

	void playVideo(const MediaId& id);
	void openSourceInSystemApp(const MediaId& id);
	void showMediaItemContextMenu(const MediaId& id, const QPoint& globalPos);
	[[nodiscard]] QString bulletedItemNameList(const std::vector<MediaId>& selection) const;
	[[nodiscard]] bool splitVideoIntoFrames(const QString& videoFilePath, const QString& outputFolder);
	// A pre-commit failure restores the complete previous frame folder.
	[[nodiscard]] bool resplitVideoIntoFrames(const MediaId& id, bool preserveExistingPreview);
	[[nodiscard]] bool ensureFramesSplit(const MediaId& id);
	[[nodiscard]] bool regeneratePreviewFromRealFrames(const QString& folderPath, int frameCount);
	[[nodiscard]] bool regeneratePreviewFor(const MediaId& id);
	void reExportAllVideos();
	void importVideoBatch(QStringList videoPaths, const QString& storageFolderPath, const QHash<MediaId, QString>& stagedPreviewDirs, const QHash<MediaId, qint64>& stagedDurations);
	std::vector<Import::PhotoResult> importPhotoBatch(LabelId labelId, const QStringList& photoPaths, Import::PhotoImportMode mode);
	LabelId createFolderLabel(const QString& name, const QString& color = {}, bool refreshList = true);
	void createLabelInteractive();
	void renameLabelInteractive(LabelId labelId);
	void setLabelColorInteractive(LabelId labelId);
	void deleteLabelInteractive(LabelId labelId);
	void openImportDialog(const QStringList& initialStaging = {});
	void scanForUntrackedFiles();
	void checkCatalogIntegrity();

	void deleteSelectedItems();
	void removeSelectedItemsFromLibrary();
	void renameSelectedItemInteractive();
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
	void renameItemInteractive(const MediaId& id);

private:
	[[nodiscard]] Catalog& libraryCatalog();
	[[nodiscard]] const Catalog& libraryCatalog() const;

	// Declared first so it outlives the other C++ members; the destructor explicitly deletes Qt children
	// that borrow it before member destruction begins.
	Library _library;
	MediaBrowserWidget* _mediaBrowser = nullptr;
	FrameViewerWindow* _frameViewer = nullptr;

	std::optional<MediaId> _contextMenuTarget;

	QAction* _deleteAction = nullptr;
	QAction* _removeFromLibraryAction = nullptr;
	QAction* _renameAction = nullptr;

	QMenu* _libraryMenu = nullptr;
	std::vector<QAction*> _recentLibraryActions;

	bool _isProcessing = false;
	bool _persistenceWarningQueued = false;
};
