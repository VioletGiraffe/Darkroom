#pragma once

#include "Core/MediaId.h"

#include <QMainWindow>

#include <memory>
#include <vector>

class QMediaPlayer;
class QAudioOutput;
class QVideoWidget;
class QSlider;
class QLabel;
class QCheckBox;
class QComboBox;
class MarkerSlider;
class Library;
class OscillatingPlayback;
struct OscillationRequest;

class VideoPlayerWindow final : public QMainWindow
{
public:
	// mediaId keys per-video metadata; derive it from videoPath for ad-hoc playback.
	VideoPlayerWindow(Library& library, const QString& videoPath, const MediaId& mediaId, QWidget* parent);
	~VideoPlayerWindow() override;

	static void restartAll();
	static void closeAll();

	// Opens a self-managing ad-hoc player.
	static void createPlayerWindow(Library& library, const QString& videoPath, QWidget* parent);

private:
	friend class OscillatingPlayback;

	void resizeAndMoveWindow();
	void togglePlayPause();
	void toggleFullScreen();
	[[nodiscard]] qint64 currentPlaybackPosition() const;
	[[nodiscard]] bool isPlaybackActive() const;
	void setPlaybackActive(bool active);
	[[nodiscard]] bool hasAbInterval() const { return _loopStart >= 0 && _loopEnd > _loopStart; }
	void exitOscillatingPlayback(bool restorePlaybackState = true);
	void updatePlaybackPositionUi(qint64 position);
	void applyEffectiveMute();
	void updateOscillationAvailability();
	void startOscillatingPlayback();
	[[nodiscard]] bool buildOscillationRequest(OscillationRequest* request, QString* error) const;
	void onOscillationPrepared();
	void onOscillationPositionChanged(qint64 position);
	// diagnostics is raw ffmpeg output for a bounded detail pane; error is the summary.
	void onOscillationFailed(const QString& error, const QString& diagnostics, qint64 displayedPosition,
		bool hasDisplayedPosition, bool shouldResumePlayback);

	void showContextMenu(const QPoint& globalPos);
	void extractFrameToLibrary(qint64 timestampMs);
	void extractFrameToFolder(qint64 timestampMs, const QString& folder);
	void repeatLastExtraction(qint64 timestampMs);

	bool eventFilter(QObject* watched, QEvent* event) override;

private:
	static std::vector<VideoPlayerWindow*> _instances;

	Library& _library;
	MediaId _mediaId;
	const QString _videoPath;

	QMediaPlayer* _player = nullptr;
	QAudioOutput* _audioOutput = nullptr;
	QVideoWidget* _videoWidget = nullptr;
	MarkerSlider* _seekSlider = nullptr;
	QLabel* _timeLabel = nullptr;
	QSlider* _volumeSlider = nullptr;
	QCheckBox* _oscillationCheck = nullptr;
	QComboBox* _oscillationCurveCombo = nullptr;
	// Borrows this window and its video sink; the destructor resets it before QObject child teardown begins.
	std::unique_ptr<OscillatingPlayback> _oscillatingPlayback;
	bool _windowPlacementDone = false;
	bool _pauseOnSeek = true;
	bool _wasPlayingBeforeSeek = false;
	bool _userMuted = false;

	qint64 _loopStart = -1;
	qint64 _loopEnd = -1;
};
