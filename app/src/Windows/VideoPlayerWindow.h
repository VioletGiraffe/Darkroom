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

class VideoPlayerWindow final : public QMainWindow
{
public:
	// mediaId keys the saved loop intervals in MetadataStore; pass the catalog's id for a tracked video, or
	// MediaId::fromFile(videoPath) for an ad-hoc one (a staging/untracked preview not in the catalog).
	VideoPlayerWindow(Library& library, const QString& videoPath, const MediaId& mediaId, QWidget* parent);
	~VideoPlayerWindow() override;

	static void restartAll();
	static void closeAll();

	// Convenience: opens a self-managing player window for the file as an ad-hoc playback (the MediaId is derived from the file, so an untracked/staged video works too)
	static void createPlayerWindow(Library& library, const QString& videoPath, QWidget* parent);

private:
	class OscillatingPlayback;
	struct OscillationRequest;

	void resizeAndMoveWindow();
	void togglePlayPause();
	void toggleFullScreen();
	[[nodiscard]] qint64 currentPlaybackPosition() const;
	[[nodiscard]] bool isPlaybackActive() const;
	void setPlaybackActive(bool active);
	void exitOscillatingPlayback(bool restorePlaybackState = true);
	void updatePlaybackPositionUi(qint64 position);
	void applyEffectiveMute();
	void updateOscillationAvailability();
	void startOscillatingPlayback();
	[[nodiscard]] bool buildOscillationRequest(OscillationRequest* request, QString* error) const;
	void onOscillationPrepared();
	void onOscillationPositionChanged(qint64 position);
	void onOscillationFailed(const QString& error, qint64 displayedPosition, bool hasDisplayedPosition, bool shouldResumePlayback);

	// The video's right-click menu: frame extraction (to a folder or into the library as an owned photo) and a
	// fullscreen toggle.
	void showContextMenu(const QPoint& globalPos);
	void extractFrameToLibrary(qint64 timestampMs);
	void extractFrameToFolder(qint64 timestampMs, const QString& folder);
	// Replays the last extraction (library or folder, same destination) at timestampMs; no-op until one has run.
	// Bound to the 'E' shortcut and the menu's "last used" item.
	void repeatLastExtraction(qint64 timestampMs);
	// The shared extraction step: runs ffmpeg, reports any failure. Returns the written file's path, empty on failure.
	[[nodiscard]] QString extractFrameInto(qint64 timestampMs, const QString& destinationFolder);

	bool eventFilter(QObject* watched, QEvent* event) override;

private:
	static std::vector<VideoPlayerWindow*> _instances;

	Library& _library;
	MediaId _mediaId; // identity of the played source video; keys its saved loops in MetadataStore
	const QString _videoPath;

	QMediaPlayer* _player = nullptr;
	QAudioOutput* _audioOutput = nullptr;
	QVideoWidget* _videoWidget = nullptr;
	MarkerSlider* _seekSlider = nullptr;
	QLabel* _timeLabel = nullptr;
	QSlider* _volumeSlider = nullptr;
	QCheckBox* _oscillationCheck = nullptr;
	QComboBox* _oscillationCurveCombo = nullptr;
	std::unique_ptr<OscillatingPlayback> _oscillatingPlayback;
	bool _windowPlacementDone = false;
	bool _pauseOnSeek = true;
	bool _wasPlayingBeforeSeek = false;
	bool _userMuted = false;

	// A-B loop interval, in milliseconds; -1 means the respective endpoint is unset.
	// Looping is active only while both are set and _loopEnd > _loopStart.
	qint64 _loopStart = -1;
	qint64 _loopEnd = -1;
};
