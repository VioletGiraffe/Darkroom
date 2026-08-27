#pragma once

#include "Core/MediaId.h"
#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QList>
#include <QSize>
#include <QString>
#include <QUrl>
#include <QWidget>
RESTORE_COMPILER_WARNINGS

#include <optional>
#include <vector>

class QAction;
class LabelSidebar;
class Library;
class MediaGrid;
class MediaItemWidget;
class PreviewFrameCountCombo;
class QLineEdit;
class QListWidgetItem;
class QObject;
class QTimer;
class SegmentedToggle;
class SortControl;

// Main-window media browser feature: sidebar, toolbar, grid, local media actions, and browser persistence.
class MediaBrowserWidget final : public QWidget
{
	Q_OBJECT
public:
	explicit MediaBrowserWidget(Library& library, QWidget* parent = nullptr);

	void saveSettings();
	// Restores filters, then schedules the initial grid build after post-show layout settles.
	void restoreSettings();

	void refreshLibraryView();
	void resetForLibrarySwitch();

	[[nodiscard]] int previewFrameCount() const;
	[[nodiscard]] std::vector<MediaId> selectedMediaItems() const;
	// Grid order, without the rows the name filter hides and without photos: what a player can browse through.
	[[nodiscard]] std::vector<MediaId> visibleVideosInViewOrder() const;

	void installGridAction(QAction* action);
	[[nodiscard]] bool isGridDragSource(const QObject* source) const;

	void deleteSelectedMediaItemsInteractive();
	void removeSelectedMediaItemsFromLibraryInteractive();
	void renameSelectedMediaItemInteractive();

signals:
	void selectionChanged();
	void inspectVideoFramesRequested(const MediaId& id);
	// An empty new path means the folder was deleted or may have been partially deleted.
	void frameFolderPathChanged(const QString& oldFolderPath, const QString& newFolderPath, const QString& newDisplayName);

private:
	struct GridViewState;

	void setupUi();
	void activateMediaItem(const MediaId& id);
	void playVideo(const MediaId& id);
	void openSourceInSystemApp(const MediaId& id);
	void showMediaItemContextMenu(const MediaId& id, const QPoint& globalPos);
	void deleteMediaItemsInteractive(const std::vector<MediaId>& selection);
	void removeMediaItemsFromLibraryInteractive(const std::vector<MediaId>& selection);
	void renameMediaItemInteractive(const MediaId& id);
	void toggleBest(const MediaId& id);
	void zoomCards(int steps);

	// Grid operations, split by what changed: which rows exist, how cards look, their order, which are hidden.
	void rebuildGridItems();
	void rebuildGridRows();
	void rebuildAllCards();
	void resortMediaGrid();
	void applyNameFilter();
	// applyNameFilter() without materializing cards, for callers that materialize later.
	void applyNameFilterToRows();

	void updateCardCanvasSizes();
	void applyCardSizeHints();
	[[nodiscard]] QSize cardSizeHintFor(bool isPhoto) const;

	[[nodiscard]] QString topAnchorKey() const;
	void scrollGridToAnchorKey(const QString& anchorKey);
	[[nodiscard]] GridViewState captureGridViewState() const;
	void restoreGridViewState(const GridViewState& state);

	[[nodiscard]] std::vector<MediaId> effectiveSelection(std::optional<MediaId> target = std::nullopt) const;
	[[nodiscard]] std::vector<MediaId> mediaItemsMatchingFilters() const;
	[[nodiscard]] MediaItemWidget* buildMediaCard(QListWidgetItem* item);
	[[nodiscard]] QList<QUrl> dragUrlsForItems(const QList<QListWidgetItem*>& items) const;

private:
	Library& _library;

	LabelSidebar*    _labelSidebar = nullptr;
	QLineEdit*       _nameFilter = nullptr;
	SegmentedToggle* _mediaTypeFilter = nullptr;
	PreviewFrameCountCombo* _previewFrameCountCombo = nullptr;
	SortControl*     _sortControl = nullptr;
	MediaGrid*       _mediaGrid = nullptr;
	QTimer*          _catalogRefreshTimer = nullptr;
	QTimer*          _gridZoomDebounce = nullptr;

	// Cards are built long after zoom and preview frame count are read, so their canvas sizes are cached here.
	QSize _photoCanvas, _videoCanvas;
};
