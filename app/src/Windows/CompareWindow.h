#pragma once

#include <QStringList>
#include <QWidget>

#include <vector>

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
	std::vector<QStringList> _folderFrames;

	std::vector<ThumbnailWidget*> _thumbnailWidgets;
	QSlider*         _slider    = nullptr;
	QLabel*          _frameLabel = nullptr;
	QTimer*          _debounceTimer = nullptr;
};
