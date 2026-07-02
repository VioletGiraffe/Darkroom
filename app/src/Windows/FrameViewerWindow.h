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
	[[nodiscard]] const QString& currentFolder() const { return m_folderPath; }

private:
	void refreshDisplay();
	void showInstruction(const QString& text);
	void showThumbnailContextMenu(const QPoint& pos);
	void zoomThumbnails(int steps);

	QString       m_folderPath;
	int           m_thumbnailSize        = 200;
	QLabel*       m_instructionLabel     = nullptr;
	QScrollArea*  m_scrollArea           = nullptr;
	QWidget*      m_thumbnailContainer   = nullptr;
	CFlowLayout*  m_thumbnailLayout      = nullptr;
	QTimer*       m_refreshDebounceTimer = nullptr;
};
