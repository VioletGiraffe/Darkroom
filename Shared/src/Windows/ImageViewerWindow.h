#pragma once

#include "Windows/RemovalAction.h"
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
// Controls are the menu bar and its shortcuts, the wheel for browsing, plus the widget's own mouse zoom and pan.
class ImageViewerWindow final : public QMainWindow
{
public:
	// Opens a self-deleting viewer on imagePaths[startIndex].
	// parent parents the failure message box shown when the image cannot be opened at all. It also owns the
	// viewer window when it is modal, since Qt blocks input to unparented windows then; otherwise the viewer
	// is parentless, for a taskbar button of its own.
	// onImageChanged reports the path browsed to; the initial image does not call it.
	// Null when the start image is missing. The window deletes itself on close: configure the result at once, never store it.
	// Paths reported to the host are current: a move concluded by followMovedMediaFiles rebases them.
	static ImageViewerWindow* showForImages(QStringList imagePaths, int startIndex, QWidget* parent,
		std::function<void(const QString& path)> onImageChanged = {});

	// Adds a host action to the Image menu; its shortcut works in fullscreen too.
	// updateForImage receives the path of the image shown, now and after each image change, to set the action's state.
	void addImageAction(QAction* action, std::function<void(const QString& path)> updateForImage);
	[[nodiscard]] QString currentImagePath() const { return _imagePaths[_index]; }

	// Called when the user leaves fullscreen, before the window state changes; cleared first, so it runs once.
	// Returning false skips the state change: a handler that closes the viewer must not let it reappear windowed.
	void setExitFullScreenHandler(std::function<bool()> handler);

	// Del and Shift+Del, for the image shown. Call at most once. After a removal the viewer shows the nearest remaining image, or closes when none is left.
	void setRemovalActions(RemovalAction onDelete, RemovalAction onShiftDelete);

	// Viewers whose current image matches close its file, keeping the frame shown; see releaseMediaFilesHeldOpen.
	static void releaseFilesMatching(const std::function<bool(const QString& path)>& isAffected);
	// Rebases the image paths by newLocation; a viewer whose closed image wasReleased reopens it there. See followMovedMediaFiles.
	static void followMovedFiles(const std::function<bool(const QString& path)>& wasReleased,
		const std::function<QString(const QString& path)>& newLocation);

	~ImageViewerWindow() override;

protected:
	bool eventFilter(QObject* watched, QEvent* event) override;
	void wheelEvent(QWheelEvent* event) override;

private:
	ImageViewerWindow(QStringList imagePaths, int startIndex, QWidget* parent);

	// The values are the step through _imagePaths.
	enum class Direction { Previous = -1, Next = 1 };

	void buildMenus();
	// Leaves the view untouched when the image cannot be loaded.
	void showImage(int index);
	void updateTitleAndCaption();
	// Skips paths that have left the disk; -1 when the list ends first.
	[[nodiscard]] int adjacentIndex(Direction direction) const;
	void toggleFullScreen();
	// The pixels for apps that paste images, the file for file managers and apps that take files.
	void copyImageAndFileToClipboard();
	void removeCurrentImage(const std::function<bool(const QString& path)>& remove);
	// Reopens the current image after releaseFilesMatching closed it; no-op otherwise.
	void resumeReleasedAnimation();

private:
	static std::vector<ImageViewerWindow*> _instances;

	QStringList _imagePaths;
	int _index = 0;
	// Only an animated image holds its file open, so only an animation is released.
	bool _animationReleased = false;
	// Touchpads and high-resolution wheels split a notch across several events.
	int _pendingWheelDelta = 0;

	std::function<void(const QString& path)> _onImageChanged;
	std::function<bool()> _exitFullScreenHandler;
	std::vector<std::function<void(const QString& path)>> _imageActionUpdaters;

	CImageViewerWidget* _view = nullptr;
	QTimer* _windowIconTimer = nullptr;
	QMenu* _imageMenu = nullptr;
	QAction* _previousAction = nullptr;
	QAction* _nextAction = nullptr;
};
