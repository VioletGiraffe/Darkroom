#pragma once

#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QFileIconProvider>
#include <QMainWindow>
#include <QString>
RESTORE_COMPILER_WARNINGS

#include <vector>

class MediaGrid;
class QAction;
class QLineEdit;
class QListWidgetItem;
class QTimer;

// Quickroom's main window: a filesystem browser showing folders and media items as a thumbnail grid.
// Double-click opens folders in place, images in ImageViewerWindow, videos in VideoPlayerWindow.
class BrowserWindow final : public QMainWindow
{
public:
	// Creates a self-deleting browser window at folder, or at the remembered folder when folder is empty.
	// selectPath, when the listed folder contains it, becomes the current item.
	static void showForFolder(const QString& folder = {}, const QString& selectPath = {});

protected:
	void closeEvent(QCloseEvent* event) override;
	// Forwards mouse back/forward buttons from the grid viewport to history navigation.
	bool eventFilter(QObject* watched, QEvent* event) override;

private:
	explicit BrowserWindow(const QString& folder);

	void setupUi();

	// Lists path into the grid; false and no state change when path is not an existing directory.
	bool listDirectory(const QString& path);
	// listDirectory plus a history push; a failed listing reverts the path edit.
	void navigateTo(const QString& path);
	void goBack();
	void goForward();
	void goUp();
	// Relists the current folder, keeping the scroll position.
	void refresh();
	void updateNavigationActions();

	void activateEntry(const QListWidgetItem* item);
	// Opens the viewer browsing all images the grid currently shows.
	void viewImage(const QString& path);
	void playVideo(const QString& path);
	void showEntryContextMenu(const QString& path, QPoint globalPos);
	// Does nothing when the path is no longer in the grid.
	void selectAndScrollToPath(const QString& path);

	[[nodiscard]] QWidget* buildTile(QListWidgetItem* item);
	void zoomTiles(int steps);
	void applyTileSize();

private:
	MediaGrid* _grid = nullptr;
	QLineEdit* _pathEdit = nullptr;
	QAction*   _backAction = nullptr;
	QAction*   _forwardAction = nullptr;
	QAction*   _upAction = nullptr;
	QTimer*    _zoomDebounce = nullptr;
	QFileIconProvider _iconProvider;

	QString _currentPath;
	std::vector<QString> _history;
	int _historyIndex = -1;
};
