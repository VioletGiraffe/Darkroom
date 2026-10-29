#pragma once

#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QStringList>
#include <QWidget>
RESTORE_COMPILER_WARNINGS

class MediaGrid;
class QListWidgetItem;
class QTimer;

class FrameViewerWindow final : public QWidget {
public:
	explicit FrameViewerWindow(QWidget* parent = nullptr);

	// Empty title uses a generic fallback; folder leaves are never displayed because they carry an identity suffix.
	void showForFolder(const QString& folderPath, const QString& title = {});
	[[nodiscard]] const QString& currentFolder() const { return _folderPath; }

private:
	void refreshDisplay();
	[[nodiscard]] QWidget* buildThumbnail(QListWidgetItem* item);
	// Browses all frames, starting from item's.
	void openInImageViewer(const QListWidgetItem& item);
	// Copies the paths of MediaGrid::effectiveSelection(item); reveals item alone.
	void showThumbnailContextMenu(QListWidgetItem& item, QPoint globalPos);
	// Reveals the frame folder; nothing without an existing one.
	void showBackgroundContextMenu(QPoint globalPos);
	void zoomThumbnails(int steps);
	void applyThumbnailSize();

private:
	QString       _folderPath;
	QStringList   _imagePaths;            // one per grid row, in row order
	int           _thumbnailSize        = 200;
	MediaGrid*    _grid                 = nullptr;
	QTimer*       _zoomDebounce         = nullptr;
};
