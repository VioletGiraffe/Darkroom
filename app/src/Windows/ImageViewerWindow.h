#pragma once

#include "Core/MediaId.h"
#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QMainWindow>
#include <QStringList>
RESTORE_COMPILER_WARNINGS

class CImageViewerWidget;
class Library;
class QAction;
class QTimer;

// Single-image viewer over the shared CImageViewerWidget, browsing a fixed list of sibling images.
// Controls are the menu bar and its shortcuts, plus the widget's own mouse zoom and pan.
class ImageViewerWindow final : public QMainWindow
{
public:
	// Opens a self-deleting viewer on imagePaths[startIndex]. A null library leaves out the library actions:
	// a frame is a file, not a catalog item. The window is parentless, for a taskbar button of its own;
	// dialogParent only parents the failure message box shown when the image cannot be opened at all.
	static void showForImages(Library* library, QStringList imagePaths, int startIndex, QWidget* dialogParent);

protected:
	bool eventFilter(QObject* watched, QEvent* event) override;

private:
	ImageViewerWindow(Library* library, QStringList imagePaths, int startIndex);

	// The values are the step through _imagePaths.
	enum class Direction { Previous = -1, Next = 1 };

	void buildMenus();
	// Leaves the view untouched when the image cannot be loaded.
	void showImage(int index);
	// Skips paths that have left the disk; -1 when the list ends first.
	[[nodiscard]] int adjacentIndex(Direction direction) const;
	[[nodiscard]] QString currentPath() const { return _imagePaths[_index]; }
	// Invalid unless the current file is a catalog item.
	[[nodiscard]] MediaId currentMediaId() const;
	void toggleBest();
	void updateLibraryActions();
	void toggleFullScreen();

private:
	Library* _library = nullptr;
	QStringList _imagePaths;
	int _index = 0;

	CImageViewerWidget* _view = nullptr;
	QTimer* _windowIconTimer = nullptr;
	QAction* _previousAction = nullptr;
	QAction* _nextAction = nullptr;
	QAction* _bestAction = nullptr;   // absent without a library
};
