#pragma once

#include "Core/Library.h"
#include "Core/LabelId.h"
#include "Core/MediaId.h"
#include "Import.h"  // Import::PhotoImportMode / PhotoResult (importPhotoBatch's interface)

#include <QHash>
#include <QMainWindow>
#include <QSet>
#include <QStringList>

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
class QMenu;
class QTimer;
class QUrl;
class QWidget;
class SegmentedToggle;
class SortControl;

class MainWindow final : public QMainWindow
{
public:
	// Loads the library before building anything (the sidebar and grid borrow it for their whole lives), asking
	// for another folder after each failure. Cancelling that leaves the window UNBUILT: nothing but
	// isLibraryLoaded() and destruction is valid on it, so main() must ask before showing it.
	explicit MainWindow(QWidget* parent = nullptr);
	~MainWindow();

	[[nodiscard]] bool isLibraryLoaded() const { return _library.isLoaded(); }

protected:
	void dragEnterEvent(QDragEnterEvent* event) override;
	void dropEvent(QDropEvent* event) override;
	void closeEvent(QCloseEvent* event) override;

private:
	// The startup half of the library flow: configured root first, then a picker after each failure until one
	// loads. False = the user cancelled. Loads through setRoot(), the same call a later switch uses.
	[[nodiscard]] bool loadInitialLibrary();

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
	// Hold the user's place in the grid across a rebuild (which clears the QListWidget, resetting its scrollbar
	// to the top) and across restarts (persisted in save/restoreSettings): topAnchorKey is the MediaId key of the
	// top-most visible card, scrollGridToAnchorKey scrolls that card back to the top if it's still present.
	[[nodiscard]] QString topAnchorKey() const;
	void scrollGridToAnchorKey(const QString& anchorKey);
	// The grid view state captured before a rebuild and reapplied after it, all keyed by MediaId so it survives
	// the item churn: the scroll anchor plus the selection and keyboard-anchor current item. Selection is
	// preserved across refreshes only - saveSettings persists just the scroll anchor across restarts.
	struct GridViewState
	{
		QString       scrollAnchorKey;
		QSet<QString> selectedKeys;
		QString       currentKey;
	};
	[[nodiscard]] GridViewState captureGridViewState() const;
	void restoreGridViewState(const GridViewState& state);
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
	// Extracts a candidate full frame set and reports ffmpeg failures; touches neither the catalog nor any
	// existing frame folder (resplitVideoIntoFrames moves that folder aside first).
	[[nodiscard]] bool splitVideoIntoFrames(const QString& videoFilePath, const QString& outputFolder);
	// Transactionally replaces an already-tracked video's frame folder. The old folder stays under a unique
	// sibling name until extraction succeeds, then either contributes its existing preview/ or is discarded
	// after a fresh preview is generated. A pre-commit failure restores the complete old folder.
	[[nodiscard]] bool resplitVideoIntoFrames(const MediaId& id, bool preserveExistingPreview);
	// If id hasn't had its full frame set extracted yet, does so now (synchronously, same as a normal split).
	// Called right before opening the frame viewer on a card; false means the viewer must not be opened.
	[[nodiscard]] bool ensureFramesSplit(const MediaId& id);
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

	enum class LibraryPickerMode { Open, CreateNew };
	// Asks for a library folder and switches to it, re-asking until the user succeeds or cancels. CreateNew
	// differs only in rejecting a folder that already holds a library, and in the wording of its dialogs.
	void pickAndSwitchLibrary(LibraryPickerMode mode);
	// Switches to an entry of the Library menu's recent list. The entry is never checked for existence up front
	// (see recentLibraries) - a stale one simply fails the switch and reports it, keeping its place in the list
	// so that re-plugging the drive is enough to make it work again.
	void openRecentLibrary(const QString& root);
	[[nodiscard]] bool switchLibraryTo(const QString& root, QString* error);
	// switchLibraryTo, reporting a failure under the given dialog title. Returns whether the switch happened.
	bool switchLibraryToOrReport(const QString& root, const QString& dialogTitle);
	// True (having said so) while a frame-extraction loop is pumping events: it holds catalog/store references
	// and a batch writer across the loop, none of which may outlive a root change.
	[[nodiscard]] bool refuseLibraryChangeWhileProcessing();
	// Refills the Library menu's recent entries; wired to its aboutToShow.
	void rebuildRecentLibraryActions();
	void schedulePersistenceFailureWarning();
	void openSettings();
	// Runs the interactive rename flow (see MediaRename.h), then refreshes the view and repoints the frame
	// viewer if it was showing the renamed video's folder.
	void renameItemInteractive(const MediaId& id);

private:
	[[nodiscard]] Catalog& libraryCatalog();
	[[nodiscard]] const Catalog& libraryCatalog() const;

	// Declared first so it outlives the other C++ members; the destructor explicitly deletes Qt children
	// that borrow it before member destruction begins.
	Library _library;
	LabelSidebar*      _labelSidebar = nullptr;
	QLineEdit*         _nameFilter   = nullptr;
	SegmentedToggle*   _mediaTypeFilter = nullptr;  // All / Videos / Photos grid filter
	QComboBox*         _previewFrameCountCombo = nullptr;
	SortControl*       _sortControl  = nullptr;
	MediaGrid*         _mediaGrid    = nullptr;
	QTimer*            _gridZoomDebounce = nullptr;
	FrameViewerWindow* _frameViewer  = nullptr;

	// The right-clicked card's id while a context menu is open; nullopt when triggered via keyboard shortcut
	// or main menu. Passed to effectiveSelection so that actions resolve to the right-click target when it
	// falls outside the grid selection.
	std::optional<MediaId> _contextMenuTarget;

	// Persistent actions for Edit menu / keyboard shortcuts. Created in setupMainMenu, reused in context menu.
	QAction* _deleteAction = nullptr;
	QAction* _removeFromLibraryAction = nullptr;
	QAction* _renameAction = nullptr;

	QMenu* _libraryMenu = nullptr;
	std::vector<QAction*> _recentLibraryActions;  // the menu's recent entries, held so each rebuild can drop the previous set

	// Guards against re-entering the frame-extraction loops (which pump events) from a new drop or menu action while one is already running.
	bool _isProcessing = false;
	bool _persistenceWarningQueued = false;
};
