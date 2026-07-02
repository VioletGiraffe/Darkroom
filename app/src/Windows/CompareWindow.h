#pragma once

#include <QStringList>
#include <QWidget>

class QLabel;
class QSlider;
class QTimer;
class ThumbnailWidget;

class CompareWindow final : public QWidget {
public:
	// folderPaths: the selected video-frame folders to compare side by side.
	explicit CompareWindow(const QStringList& folderPaths, QWidget* parent = nullptr);
	~CompareWindow();

protected:
	void resizeEvent(QResizeEvent* event) override;

private:
	void startFrameLoadTimer();
	void loadCurrentFrame();

private:
	// Per-folder sorted image file lists.
	QList<QStringList> m_folderFrames;

	QList<ThumbnailWidget*> m_thumbnailWidgets;
	QSlider*         m_slider    = nullptr;
	QLabel*          m_frameLabel = nullptr;
	QTimer*          m_debounceTimer = nullptr;
};
