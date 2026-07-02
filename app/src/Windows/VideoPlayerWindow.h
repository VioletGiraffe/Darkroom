#pragma once

#include "Core/VideoId.h"

#include <QList>
#include <QMainWindow>

class QMediaPlayer;
class QVideoWidget;

class VideoPlayerWindow final : public QMainWindow
{
public:
	// videoId keys the saved loop intervals in MetadataStore; pass the catalog's id for a tracked video, or
	// VideoId::fromFile(videoPath) for an ad-hoc one (a staging/untracked preview not in the catalog).
	VideoPlayerWindow(const QString& videoPath, const VideoId& videoId, QWidget* parent);
	~VideoPlayerWindow() override;

	static void restartAll();

	static void closeAll();

private:
	void resizeAndMoveWindow();
	void togglePlayPause();

	bool eventFilter(QObject* watched, QEvent* event) override;

private:
	static QList<VideoPlayerWindow*> _instances;

	VideoId _videoId; // identity of the played source video; keys its saved loops in MetadataStore

	QMediaPlayer* _player = nullptr;
	QVideoWidget* _videoWidget = nullptr;
	bool _pauseOnSeek = true;
	bool _wasPlayingBeforeSeek = false;

	// A-B loop interval, in milliseconds; -1 means the respective endpoint is unset.
	// Looping is active only while both are set and _loopEnd > _loopStart.
	qint64 _loopStart = -1;
	qint64 _loopEnd = -1;
};
