#pragma once

#include "Core/MediaId.h"

#include <QMainWindow>

#include <vector>

class QMediaPlayer;
class QVideoWidget;

class VideoPlayerWindow final : public QMainWindow
{
public:
	// mediaId keys the saved loop intervals in MetadataStore; pass the catalog's id for a tracked video, or
	// MediaId::fromFile(videoPath) for an ad-hoc one (a staging/untracked preview not in the catalog).
	VideoPlayerWindow(const QString& videoPath, const MediaId& mediaId, QWidget* parent);
	~VideoPlayerWindow() override;

	static void restartAll();

	static void closeAll();

private:
	void resizeAndMoveWindow();
	void togglePlayPause();

	bool eventFilter(QObject* watched, QEvent* event) override;

private:
	static std::vector<VideoPlayerWindow*> _instances;

	MediaId _mediaId; // identity of the played source video; keys its saved loops in MetadataStore

	QMediaPlayer* _player = nullptr;
	QVideoWidget* _videoWidget = nullptr;
	bool _pauseOnSeek = true;
	bool _wasPlayingBeforeSeek = false;

	// A-B loop interval, in milliseconds; -1 means the respective endpoint is unset.
	// Looping is active only while both are set and _loopEnd > _loopStart.
	qint64 _loopStart = -1;
	qint64 _loopEnd = -1;
};
