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

	// title names the window (the item's display name); empty falls back to a generic title, as on the empty-folder
	// clear. The folder leaf is deliberately not used - it carries a hash suffix that must not surface to the user.
	void showForFolder(const QString& folderPath, const QString& title = {});
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
