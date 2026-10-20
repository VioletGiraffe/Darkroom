#pragma once

#include "Windows/PathDeletion.h"
#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QFileIconProvider>
#include <QMainWindow>
#include <QSet>
#include <QString>
#include <QStringList>
RESTORE_COMPILER_WARNINGS

#include <functional>
#include <vector>

class MediaGrid;
class QAction;
class QDragEnterEvent;
class QDragMoveEvent;
class QDropEvent;
class QFileSystemWatcher;
class QLineEdit;
class QListWidgetItem;
class QTimer;
class SortControl;

// Quickroom's main window: a filesystem browser showing folders and media items as a thumbnail grid.
// Double-click opens folders in place, images in ImageViewerWindow, videos in VideoPlayerWindow.
class BrowserWindow final : public QMainWindow
{
public:
	// Creates a self-deleting browser window at folder, or at the remembered folder when folder is empty.
	// selectPath, when the listed folder contains it, becomes the current item.
	static void showForFolder(const QString& folder = {}, const QString& selectPath = {});

protected:
	// Forwards mouse back/forward buttons from the grid viewport to history navigation.
	bool eventFilter(QObject* watched, QEvent* event) override;

	// Files dropped anywhere on the window are copied or moved into the folder shown, or into the folder tile under the cursor.
	void dragEnterEvent(QDragEnterEvent* event) override;
	void dragMoveEvent(QDragMoveEvent* event) override;
	void dropEvent(QDropEvent* event) override;

private:
	explicit BrowserWindow(const QString& folder);

	void setupUi();

	// Lists path into the grid and watches it; false and no state change when path cannot be listed.
	bool listDirectory(const QString& path);
	// listDirectory plus a history push; a failed listing reverts the path edit.
	void navigateTo(const QString& path);
	void goBack();
	void goForward();
	void goUp();
	// Brings the grid in line with the folder on disk; unchanged entries keep their tiles.
	// A folder that is gone navigates to its nearest existing ancestor.
	void refresh();
	void updateNavigationActions();

	void activateEntry(const QListWidgetItem* item);
	// Opens the viewer browsing all images the grid currently shows.
	void viewImage(const QString& path);
	void playVideo(const QString& path);
	void showEntryContextMenu(const QString& path, bool isFolder, QPoint globalPos);
	void showBackgroundContextMenu(QPoint globalPos);
	// Does nothing when the path is no longer in the grid.
	void selectAndScrollToPath(const QString& path);
	// selectAndScrollToPath, safe to call after this window is gone, as entryRemover.
	[[nodiscard]] std::function<void(const QString& path)> pathSelector();

	[[nodiscard]] QStringList selectedPaths() const;
	void deletePaths(const QStringList& paths, PathDeletion::Mode mode);
	// Drops the entries of paths; the entry that followed the last of them becomes current.
	void removeEntries(const QSet<QString>& paths);
	// removeEntries for one path, safe to call after this window is gone: viewers and players can outlive it.
	[[nodiscard]] std::function<void(const QString& path)> entryRemover();
	void updateStatusCounts();

	// Copies or moves the files on the clipboard into folder; a cut is moved and taken off the clipboard.
	void pasteInto(const QString& folder);
	// Fades the tiles of the files cut to the clipboard, by this or any other app.
	void updateCutMarks();
	[[nodiscard]] bool isCut(const QString& path) const;

	// The folder a drop lands in. Empty where it makes no sense: the grid's own items back into their folder or into themselves.
	[[nodiscard]] QString dropTargetFolder(const QDropEvent& event, const QStringList& sources) const;
	// Follows file managers: a move within one volume and a copy across volumes, unless the copy or move key is held.
	[[nodiscard]] Qt::DropAction dropAction(const QDropEvent& event) const;

	[[nodiscard]] QWidget* buildTile(QListWidgetItem* item);
	void zoomTiles(int steps);
	void applyTileSize();

private:
	MediaGrid* _grid = nullptr;
	QLineEdit* _pathEdit = nullptr;
	SortControl* _sortControl = nullptr;
	QAction*   _backAction = nullptr;
	QAction*   _forwardAction = nullptr;
	QAction*   _upAction = nullptr;
	QTimer*    _zoomDebounce = nullptr;
	QTimer*    _refreshDebounce = nullptr;
	QFileSystemWatcher* _folderWatcher = nullptr;
	QFileIconProvider _iconProvider;

	int _tileSize = -1;
	QString _currentPath;
	std::vector<QString> _history;
	int _historyIndex = -1;

	QSet<QString> _cutPathKeys; // pathComparisonKey of each cut file
	// Taken when a drag enters; folder tiles count as the shown folder's volume.
	bool _dropSourcesOnShownVolume = false;
};
