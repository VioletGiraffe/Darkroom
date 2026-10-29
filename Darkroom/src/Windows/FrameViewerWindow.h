#pragma once

#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QStringList>
#include <QWidget>
RESTORE_COMPILER_WARNINGS

class MediaGrid;
class QLabel;
class QListWidgetItem;
class QTimer;
class ThumbnailWidget;

class FrameViewerWindow final : public QWidget {
public:
	explicit FrameViewerWindow(QWidget* parent = nullptr);

	// Empty title uses a generic fallback; folder leaves are never displayed because they carry an identity suffix.
	void showForFolder(const QString& folderPath, const QString& title = {});
	[[nodiscard]] const QString& currentFolder() const { return _folderPath; }

private:
	void refreshDisplay();
	void showInstruction(const QString& text);
	[[nodiscard]] QWidget* buildThumbnail(QListWidgetItem* item);
	void showThumbnailContextMenu(ThumbnailWidget& thumbnail, const QString& path, const QPoint& pos);
	void zoomThumbnails(int steps);
	void applyThumbnailSize();

private:
	QString       _folderPath;
	QStringList   _imagePaths;            // one per grid row, in row order
	int           _thumbnailSize        = 200;
	QLabel*       _instructionLabel     = nullptr;
	MediaGrid*    _grid                 = nullptr;
	QTimer*       _zoomDebounce         = nullptr;
};
