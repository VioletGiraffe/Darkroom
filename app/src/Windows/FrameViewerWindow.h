#pragma once

#include <QWidget>

class CFlowLayout;
class QLabel;
class QScrollArea;
class QTimer;
class ThumbnailWidget;

class FrameViewerWindow final : public QWidget {
public:
	explicit FrameViewerWindow(QWidget* parent = nullptr);

	void showForFolder(const QString& folderPath);
	[[nodiscard]] const QString& currentFolder() const { return _folderPath; }

private:
	void refreshDisplay();
	void showInstruction(const QString& text);
	void showThumbnailContextMenu(const QPoint& pos);
	void zoomThumbnails(int steps);

	QString       _folderPath;
	int           _thumbnailSize        = 200;
	QLabel*       _instructionLabel     = nullptr;
	QScrollArea*  _scrollArea           = nullptr;
	QWidget*      _thumbnailContainer   = nullptr;
	CFlowLayout*  _thumbnailLayout      = nullptr;
	QTimer*       _refreshDebounceTimer = nullptr;
};
