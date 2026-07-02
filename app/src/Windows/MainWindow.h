#pragma once

#include "Core/VideoId.h"

#include <QHash>
#include <QList>
#include <QMainWindow>
#include <QStringList>

class FrameViewerWindow;
class LabelSidebar;
class QComboBox;
class QLineEdit;
class QListWidget;
class QTimer;
class QWidget;
class SortControl;

class MainWindow final : public QMainWindow
{
public:
	MainWindow(QWidget* parent = nullptr);
	~MainWindow();
	static MainWindow* instance();

protected:
	void dragEnterEvent(QDragEnterEvent* event) override;
	void dropEvent(QDropEvent* event) override;
	void closeEvent(QCloseEvent* event) override;

private:
	void setupUI();
	void setupMainMenu();

	void saveSettings();
	void restoreSettings();

	void playVideo(const VideoId& id, bool systemPlayer);
	void refreshVideoGrid();
	// Refreshes the grid AND the label sidebar (labels/counts). Use after structural changes (add/delete/
	// rename a video, create a label); plain refreshVideoGrid is for filter/sort/zoom changes.
	void refreshLibraryView();
	// Reorders the existing grid cards to the current sort settings without rebuilding them (no
	// thumbnail re-decode). Use for sort/order changes; structural changes still call refreshVideoGrid.
	void resortVideoGrid();
	// Hides grid cards whose name doesn't match the toolbar name-filter box and renumbers the visible
	// ones. Cheap (no catalog rebuild, no thumbnail decode), so it runs on every keystroke; refreshVideoGrid
	// also calls it after a rebuild to apply the active filter to the freshly built cards.
	void applyNameFilter();
	// Ctrl+wheel handler from the cards: steps the preview image height and rebuilds the grid (debounced).
	void zoomCards(int steps);
	void showVideoContextMenu(const VideoId& id, const QPoint& globalPos);
	// If id is part of the current multi-selection, returns all selected videos' ids; otherwise just id alone.
	[[nodiscard]] QList<VideoId> effectiveSelection(const VideoId& id) const;
	void splitVideoIntoFrames(const QString& videoFilePath, const QString& outputFolder);
	// Wipes and fully re-extracts an already-tracked video's frames. preserveExistingPreview controls what
	// happens to its preview/ subfolder across that wipe: true carries the existing one through unchanged
	// (used by ensureFramesSplit, whose preview/ is still fresh from ingestion); false regenerates a fresh
	// one from the new real frames once the split succeeds (used by reExportAllVideos and the integrity
	// tool's ghost reimport, where the old preview/ may be stale or the source content may have changed).
	void resplitVideoIntoFrames(const QString& videoFilePath, const QString& outputFolder, bool preserveExistingPreview);
	// If id hasn't had its full frame set extracted yet, does so now (synchronously, same as a normal split).
	// Called right before opening the frame viewer on a card - the one and only on-demand split trigger.
	void ensureFramesSplit(const VideoId& id);
	// One-time migration, run at startup before the first refreshVideoGrid: every already-split video that
	// predates this feature has real frames but no preview/ subfolder yet - the grid now reads only from
	// preview/, so back-fill it via a plain file copy (no ffmpeg needed; real frames already exist).
	void backfillMissingPreviews();
	void processVideoFile(const QString& videoPath, const QString& collectionPath, const QString& stagedPreviewDir, bool overwriteAllExisting = false);
	void reExportAllVideos();
	void processBatch(QStringList videoPaths, const QString& collectionPath, const QHash<VideoId, QString>& stagedPreviewDirs);
	bool createCollection(const QString& name, bool refreshList = true);
	// Sidebar "+ Add label": prompts for a name and creates a folder-backed label (a collection folder).
	void createLabelInteractive();
	// Sidebar label right-click menu: each prompts/confirms, mutates the Catalog, then refreshes the view.
	void renameLabelInteractive(const QString& labelId);
	void setLabelColorInteractive(const QString& labelId);
	void deleteLabelInteractive(const QString& labelId);
	// Opens the Quick Import dialog; initialStaging pre-fills the staging area (used by scanForUntrackedFiles).
	void quickImportToCollections(const QStringList& initialStaging = {});
	// Tools menu: recursively scans a chosen folder for supported videos not yet tracked by any collection.
	void scanForUntrackedFiles();
	// Tools menu: scans the catalog against disk for drift (relinkable placeholders, untracked frame
	// folders, ghost entries) and lets the user resolve each finding via IntegrityCheckDialog.
	void checkCatalogIntegrity();

	// Best helpers (Best is a label in the Catalog; these are thin wrappers over it)
	[[nodiscard]] static bool isInBest(const VideoId& id);
	void toggleBestFolder(const VideoId& id);

	void openSettings();
	void renameVideoInteractive(const VideoId& id);
	void renameVideo(const VideoId& oldId, const QString& newFolderPath);
	void deleteFolderRecursively(const QString& folderPath);

private:
	LabelSidebar*      m_labelSidebar = nullptr;
	QLineEdit*         m_nameFilter   = nullptr;
	QComboBox*         m_previewFrameCountCombo = nullptr;
	SortControl*       m_sortControl  = nullptr;
	QListWidget*       m_videoGrid    = nullptr;
	QTimer*            m_gridZoomDebounce = nullptr;
	FrameViewerWindow* m_frameViewer  = nullptr;

	// Guards against re-entering the frame-extraction loops (which pump events) from a new drop or menu action while one is already running.
	bool m_isProcessing = false;
};
