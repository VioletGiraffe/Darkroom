#pragma once

#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QMainWindow>
#include <QStringList>
RESTORE_COMPILER_WARNINGS

#include <functional>
#include <vector>

class CImageViewerWidget;
class QAction;
class QMenu;
class QTimer;

// Single-image viewer over the shared CImageViewerWidget, browsing a fixed list of sibling images.
// Controls are the menu bar and its shortcuts, plus the widget's own mouse zoom and pan.
class ImageViewerWindow final : public QMainWindow
{
public:
	// Opens a self-deleting viewer on imagePaths[startIndex].
	// parent parents the failure message box shown when the image cannot be opened at all. It also owns the
	// viewer window when it is modal, since Qt blocks input to unparented windows then; otherwise the viewer
	// is parentless, for a taskbar button of its own.
	// onImageChanged reports the browsed-to entry of imagePaths by index; the initial image does not call it.
	// Null when the start image is missing. The window deletes itself on close: configure the result at once, never store it.
	static ImageViewerWindow* showForImages(QStringList imagePaths, int startIndex, QWidget* parent,
		std::function<void(int index)> onImageChanged = {});

	// Adds a host action to the Image menu; its shortcut works in fullscreen too.
	// updateForImage receives the index of the image shown, now and after each image change, to set the action's state.
	void addImageAction(QAction* action, std::function<void(int index)> updateForImage);
	// Index into the imagePaths given to showForImages.
	[[nodiscard]] int currentImageIndex() const { return _index; }

	// Called when the user leaves fullscreen, before the window state changes; cleared first, so it runs once.
	// Returning false skips the state change: a handler that closes the viewer must not let it reappear windowed.
	void setExitFullScreenHandler(std::function<bool()> handler);

	// What Del or Shift+Del does to the image shown, in the host's terms.
	struct RemovalAction
	{
		QString text; // menu wording
		// index is into the imagePaths given to showForImages. Returns whether the image left the host's list.
		std::function<bool(int index)> remove;
	};
	// Call at most once. After a removal the viewer shows the nearest remaining image, or closes when none is left.
	void setRemovalActions(RemovalAction onDelete, RemovalAction onShiftDelete);

	// Viewers whose current image matches close its file, keeping the frame shown; see releaseMediaFilesHeldOpen.
	static void releaseFilesMatching(const std::function<bool(const QString& path)>& isAffected);

	~ImageViewerWindow() override;

protected:
	bool eventFilter(QObject* watched, QEvent* event) override;

private:
	ImageViewerWindow(QStringList imagePaths, int startIndex, QWidget* parent);

	// The values are the step through _imagePaths.
	enum class Direction { Previous = -1, Next = 1 };

	void buildMenus();
	// Leaves the view untouched when the image cannot be loaded.
	void showImage(int index);
	// Skips paths that have left the disk; -1 when the list ends first.
	[[nodiscard]] int adjacentIndex(Direction direction) const;
	[[nodiscard]] QString currentPath() const { return _imagePaths[_index]; }
	void toggleFullScreen();
	// The pixels for apps that paste images, the file for file managers and apps that take files.
	void copyImageAndFileToClipboard();
	void removeCurrentImage(const std::function<bool(int index)>& remove);

private:
	static std::vector<ImageViewerWindow*> _instances;

	QStringList _imagePaths;
	int _index = 0;

	std::function<void(int index)> _onImageChanged;
	std::function<bool()> _exitFullScreenHandler;
	std::vector<std::function<void(int index)>> _imageActionUpdaters;

	CImageViewerWidget* _view = nullptr;
	QTimer* _windowIconTimer = nullptr;
	QMenu* _imageMenu = nullptr;
	QAction* _previousAction = nullptr;
	QAction* _nextAction = nullptr;
};
