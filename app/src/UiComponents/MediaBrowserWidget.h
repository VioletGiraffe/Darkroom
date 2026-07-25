#pragma once

#include "Core/MediaId.h"

#include <QList>
#include <QString>
#include <QUrl>
#include <QWidget>

#include <optional>
#include <vector>

class QAction;
class LabelSidebar;
class Library;
class MediaGrid;
class MediaItemWidget;
class QComboBox;
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
	// Restores filters and performs the initial grid build.
	void restoreSettings();

	void refreshMediaGrid();
	void refreshLibraryView();
	void resetForLibrarySwitch();

	[[nodiscard]] int previewFrameCount() const;
	[[nodiscard]] std::vector<MediaId> selectedMediaItems() const;

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
	void resortMediaGrid();
	void applyNameFilter();

	[[nodiscard]] QString topAnchorKey() const;
	void scrollGridToAnchorKey(const QString& anchorKey);
	[[nodiscard]] GridViewState captureGridViewState() const;
	void restoreGridViewState(const GridViewState& state);

	[[nodiscard]] std::vector<MediaId> effectiveSelection(std::optional<MediaId> target = std::nullopt) const;
	[[nodiscard]] std::vector<MediaId> mediaItemsMatchingFilters() const;
	[[nodiscard]] MediaItemWidget* buildMediaCard(
		const MediaId& id, bool isBest, const QSize& photoCanvas, const QSize& videoCanvas, int previewFrameCount);
	[[nodiscard]] QList<QUrl> dragUrlsForItems(const QList<QListWidgetItem*>& items) const;

private:
	Library& _library;

	LabelSidebar*    _labelSidebar = nullptr;
	QLineEdit*       _nameFilter = nullptr;
	SegmentedToggle* _mediaTypeFilter = nullptr;
	QComboBox*       _previewFrameCountCombo = nullptr;
	SortControl*     _sortControl = nullptr;
	MediaGrid*       _mediaGrid = nullptr;
	QTimer*          _catalogRefreshTimer = nullptr;
	QTimer*          _gridZoomDebounce = nullptr;
};
