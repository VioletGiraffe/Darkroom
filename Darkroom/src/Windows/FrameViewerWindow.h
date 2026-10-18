#pragma once

#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QStringList>
#include <QWidget>
RESTORE_COMPILER_WARNINGS

class CFlowLayout;
class QLabel;
class QScrollArea;
class QTimer;
class ThumbnailWidget;

class FrameViewerWindow final : public QWidget {
public:
	explicit FrameViewerWindow(QWidget* parent = nullptr);

	// Empty title uses a generic fallback; folder leaves are never displayed because they contain identity hashes.
	void showForFolder(const QString& folderPath, const QString& title = {});
	[[nodiscard]] const QString& currentFolder() const { return _folderPath; }

private:
	void refreshDisplay();
	void showInstruction(const QString& text);
	void showThumbnailContextMenu(const QPoint& pos);
	void zoomThumbnails(int steps);

private:
	QString       _folderPath;
	QStringList   _imagePaths;
	int           _thumbnailSize        = 200;
	QLabel*       _instructionLabel     = nullptr;
	QScrollArea*  _scrollArea           = nullptr;
	QWidget*      _thumbnailContainer   = nullptr;
	CFlowLayout*  _thumbnailLayout      = nullptr;
	QTimer*       _refreshDebounceTimer = nullptr;
};
