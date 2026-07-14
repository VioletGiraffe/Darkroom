#pragma once

#include "Core/Library.h"
#include "Core/LabelId.h"
#include "Core/MediaId.h"
#include "Import.h"  // Import::PhotoImportMode / PhotoResult (importPhotoBatch's interface)

#include <QHash>
#include <QMainWindow>
#include <QStringList>

#include <memory>
#include <optional>
#include <vector>

class Catalog;
class FrameViewerWindow;
class LabelSidebar;
class MediaGrid;
class MediaItemWidget;
class QAction;
class QComboBox;
class QLineEdit;
class QListWidgetItem;
class QTimer;
class QUrl;
class QWidget;
class SegmentedToggle;
class SortControl;

class MainWindow final : public QMainWindow
{
public:
	// Opens the configured library, offering a folder picker after any load failure. Returns null only when
	// the user cancels recovery, so main() never needs to know about Library construction or validation.
	[[nodiscard]] static std::unique_ptr<MainWindow> createWithInitialLibrary(QWidget* parent = nullptr);
	~MainWindow();

protected:
	void dragEnterEvent(QDragEnterEvent* event) override;
	void dropEvent(QDropEvent* event) override;
	void closeEvent(QCloseEvent* event) override;

private:
	explicit MainWindow(Library&& library, QWidget* parent);

	void setupUI();
	void setupMainMenu();

	void saveSettings();
	void restoreSettings();

	// Opens the video in the built-in player window (after checking the source file exists).
	void playVideo(const MediaId& id);
	// Opens the item's source file in its OS-associated application - the system player for a video, the
	// system image viewer for a photo (after checking the file exists).
	void openSourceInSystemApp(const MediaId& id);
	void refreshMediaGrid();
	// The media items the structural (grid-rebuilding) filters select: the sidebar's label filter (AND/OR)
	// plus the header's All/Videos/Photos switch. The name filter is deliberately absent here - it's a
	// view-level hide/show over the already-built cards (applyNameFilter).
	[[nodiscard]] std::vector<MediaId> mediaItemsMatchingFilters() const;
	// Builds and wires one grid card for refreshMediaGrid: thumbnails, star, duration pill, label dots and
	// every callback. Returns null for a video whose preview/ is missing or empty - such an item gets no
	// card at all (rather than a frameless ghost one).
	[[nodiscard]] MediaItemWidget* buildMediaCard(const MediaId& id, bool isBest, const QSize& photoCanvas, const QSize& videoCanvas, int previewFrameCount);
	// Refreshes the grid AND the label sidebar (labels/counts). Use after structural changes (add/delete/
	// rename an item, create a label); plain refreshMediaGrid is for filter/sort/zoom changes.
	void refreshLibraryView();
	// Reorders the existing grid cards to the current sort settings without rebuilding them (no
	// thumbnail re-decode). Use for sort/order changes; structural changes still call refreshMediaGrid.
	void resortMediaGrid();
	// Hides grid cards whose name doesn't match the toolbar name-filter box and renumbers the visible
	// ones. Cheap (no catalog rebuild, no thumbnail decode), so it runs on every keystroke; refreshMediaGrid
	// also calls it after a rebuild to apply the active filter to the freshly built cards.
	void applyNameFilter();
	// Ctrl+wheel handler from the cards: steps the preview image height and rebuilds the grid (debounced).
	void zoomCards(int steps);
	void showMediaItemContextMenu(const MediaId& id, const QPoint& globalPos);
	// With id: if id is part of the current multi-selection, returns all selected ids, otherwise just id.
	// Without id (nullopt): returns the raw grid selection (may be empty).
	[[nodiscard]] std::vector<MediaId> effectiveSelection(std::optional<MediaId> id = std::nullopt) const;
	// Bulleted list of the selection's item names for confirmation dialogs, capped so a huge selection stays readable.
	[[nodiscard]] QString bulletedItemNameList(const std::vector<MediaId>& selection) const;
	// Source-file URLs for the given grid items, handed to MediaGrid to export them when a card is dragged out
	// (a multi-selection drags every selected file). Missing files (e.g. a referenced item on an unmounted
	// drive) are skipped.
	[[nodiscard]] QList<QUrl> dragUrlsForItems(const QList<QListWidgetItem*>& items) const;
	void splitVideoIntoFrames(const QString& videoFilePath, const QString& outputFolder);
	// Wipes and fully re-extracts an already-tracked video's frames. preserveExistingPreview controls what
	// happens to its preview/ subfolder across that wipe: true carries the existing one through unchanged
	// (used by ensureFramesSplit, whose preview/ is still fresh from import); false regenerates a fresh
	// one from the new real frames once the split succeeds (used by reExportAllVideos and the integrity
	// tool's ghost reimport, where the old preview/ may be stale or the source content may have changed).
	void resplitVideoIntoFrames(const QString& videoFilePath, const QString& outputFolder, bool preserveExistingPreview);
	// If id hasn't had its full frame set extracted yet, does so now (synchronously, same as a normal split).
	// Called right before opening the frame viewer on a card - the one and only on-demand split trigger.
	void ensureFramesSplit(const MediaId& id);
	// Rebuilds <folder>/preview from the video's own real frames (plain copy, no ffmpeg, no source needed).
	// Returns false and touches nothing when there are no real frames to sample. Used by the integrity tool's
	// INVISIBLE recovery (regeneratePreviewFor).
	bool regeneratePreviewFromRealFrames(const QString& folderPath, int frameCount);
	// Integrity resolution for INVISIBLE: restores a video's preview from its real frames if present (which also
	// marks it fully split), else re-extracts from its source video. Returns whether a preview exists afterwards.
	[[nodiscard]] bool regeneratePreviewFor(const MediaId& id);
	void reExportAllVideos();
	// stagedPreviewDirs / stagedDurations map each staged video's MediaId to, respectively, the temp dir whose
	// preview/ holds its already-extracted frames (reused by copy - see Import::importVideo) and the duration
	// the Import dialog already probed for it; a video absent from a map is handled fresh (re-extracted / re-probed).
	void importVideoBatch(QStringList videoPaths, const QString& storageFolderPath, const QHash<MediaId, QString>& stagedPreviewDirs, const QHash<MediaId, qint64>& stagedDurations);
	// The photo counterpart to importVideoBatch: imports each photo under the label via Import::importPhoto,
	// reports errors, applies a referenced photo's initial label (it has no storage folder to derive it
	// from), and refreshes the view. Returns one result per path, in order - the Import dialog's bookkeeping
	// branches on them (see ImportDialog::Callbacks::importPhotosRequested).
	std::vector<Import::PhotoResult> importPhotoBatch(LabelId labelId, const QStringList& photoPaths, Import::PhotoImportMode mode);
	// Creates a folder-backed label and its storage folder on disk; returns the created-or-existing label's id, None if refused.
	LabelId createFolderLabel(const QString& name, const QString& color = {}, bool refreshList = true);
	// Sidebar "Create label": prompts for a name and creates a folder-backed label (a storage folder).
	void createLabelInteractive();
	// Sidebar label right-click menu: each prompts/confirms, mutates the Catalog, then refreshes the view.
	void renameLabelInteractive(LabelId labelId);
	void setLabelColorInteractive(LabelId labelId);
	void deleteLabelInteractive(LabelId labelId);
	// Opens the Import dialog; initialStaging pre-fills the staging area (used by scanForUntrackedFiles).
	void openImportDialog(const QStringList& initialStaging = {});
	// Tools menu: recursively scans a chosen folder for supported media (videos and photos) not tracked by the
	// catalog, sending any found straight to the Import dialog for staging.
	void scanForUntrackedFiles();
	// Tools menu: scans the catalog against disk for drift (untracked frame folders and broken video
	// entries) and lets the user resolve each finding via IntegrityCheckDialog.
	void checkCatalogIntegrity();

	// Best helpers (Best is a label in the Catalog; these are thin wrappers over it)
	[[nodiscard]] bool isInBest(const MediaId& id) const;
	void toggleBest(const MediaId& id);

	void deleteSelectedItems();
	void removeSelectedItemsFromLibrary();
	void renameSelectedItemInteractive();
	void updateEditActions();

	void openLibrary();
	[[nodiscard]] bool switchLibraryTo(const QString& root, QString* error);
	void openSettings();
	// Runs the interactive rename flow (see MediaRename.h), then refreshes the view and repoints the frame
	// viewer if it was showing the renamed video's folder.
	void renameItemInteractive(const MediaId& id);

private:
	[[nodiscard]] Catalog& libraryCatalog();
	[[nodiscard]] const Catalog& libraryCatalog() const;

	// Declared first so it outlives the other C++ members; the destructor explicitly deletes Qt children
	// that borrow it before member destruction begins.
	Library m_library;
	LabelSidebar*      m_labelSidebar = nullptr;
	QLineEdit*         m_nameFilter   = nullptr;
	SegmentedToggle*   m_mediaTypeFilter = nullptr;  // All / Videos / Photos grid filter
	QComboBox*         m_previewFrameCountCombo = nullptr;
	SortControl*       m_sortControl  = nullptr;
	MediaGrid*         m_mediaGrid    = nullptr;
	QTimer*            m_gridZoomDebounce = nullptr;
	FrameViewerWindow* m_frameViewer  = nullptr;

	// The right-clicked card's id while a context menu is open; nullopt when triggered via keyboard shortcut
	// or main menu. Passed to effectiveSelection so that actions resolve to the right-click target when it
	// falls outside the grid selection.
	std::optional<MediaId> m_contextMenuTarget;

	// Persistent actions for Edit menu / keyboard shortcuts. Created in setupMainMenu, reused in context menu.
	QAction* m_deleteAction = nullptr;
	QAction* m_removeFromLibraryAction = nullptr;
	QAction* m_renameAction = nullptr;

	// Guards against re-entering the frame-extraction loops (which pump events) from a new drop or menu action while one is already running.
	bool m_isProcessing = false;
};
