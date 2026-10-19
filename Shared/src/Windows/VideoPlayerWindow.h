#pragma once

#include "Core/MediaId.h"
#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QMainWindow>
#include <QString>
#include <QStringList>
RESTORE_COMPILER_WARNINGS

#include <functional>
#include <memory>
#include <optional>
#include <vector>

class QMediaPlayer;
class QAudioOutput;
class QVideoWidget;
class QSlider;
class QLabel;
class QCheckBox;
class QComboBox;
class QPushButton;
class MarkerSlider;
class MetadataStore;
class OscillatingPlayback;
struct OscillationRequest;

class VideoPlayerWindow final : public QMainWindow
{
public:
	// Called at each metadata read and write: the store it returns can be replaced between calls.
	using MetadataStoreProvider = std::function<MetadataStore*()>;

	// metadataStore persists speed and saved loops per video; without it they last until another video loads.
	VideoPlayerWindow(const QString& videoPath, MetadataStoreProvider metadataStore, QWidget* parent);
	~VideoPlayerWindow() override;

	// Previous/next navigation through videoPaths; it stays inert unless they include the video playing.
	// onNavigated reports the index navigated to; the initial video does not call it.
	void setNavigation(QStringList videoPaths, std::function<void(int index)> onNavigated);

	// A fixed frame destination offered beside "Extract frame to folder...", and repeated by E when used last.
	struct FrameTarget
	{
		QString text;       // menu wording
		QString repeatText; // menu wording when E repeats it
		// Returns the folder to extract into, empty after reporting why there is none.
		std::function<QString()> prepareFolder;
		std::function<void(const QString& framePath)> frameWritten;
	};
	// Call at most once.
	void setFrameTarget(FrameTarget target);

	// What Del or Shift+Del does to the playing item, in the host's terms.
	struct RemovalAction
	{
		QString text; // menu wording
		// Returns whether the item left the host's list.
		std::function<bool(const MediaId& id, const QString& videoPath)> remove;
	};
	// Call at most once. After a removal the player moves to an adjacent item, or closes when it has none.
	void setRemovalActions(RemovalAction onDelete, RemovalAction onShiftDelete);

	static void restartAll();
	static void closeAll();
	// Players whose video matches drop their source and stay open without it, see releaseMediaFilesHeldOpen.
	static void releaseFilesMatching(const std::function<bool(const QString& path)>& isAffected);
	// Rebases the navigation paths by newLocation; a player whose dropped video wasReleased resumes it there. See followMovedMediaFiles.
	static void followMovedFiles(const std::function<bool(const QString& path)>& wasReleased,
		const std::function<QString(const QString& path)>& newLocation);

	// Opens a self-managing player.
	static VideoPlayerWindow* createPlayerWindow(const QString& videoPath, QWidget* parent, MetadataStoreProvider metadataStore = {});

private:
	friend class OscillatingPlayback;

	// The values are the step through _navigationPaths.
	enum class Direction { Previous = -1, Next = 1 };

	// Everything per-file lives here; the constructor is the first caller.
	void loadFile(const QString& videoPath);
	// Null without a provider.
	[[nodiscard]] MetadataStore* currentMetadataStore() const;
	// Skips entries cleared by a removal or gone from the disk; -1 when the list ends first.
	[[nodiscard]] int adjacentIndex(Direction direction) const;
	void loadAdjacentFile(Direction direction);
	void performRemoval(const RemovalAction& action);
	// Next item, else previous, else closes the window.
	void leaveRemovedItem();
	// Loads videoPath in the playback state releaseFilesMatching recorded.
	void resumeReleasedFile(const QString& videoPath);
	void resizeAndMoveWindow();
	void togglePlayPause();
	void toggleFullScreen();
	[[nodiscard]] qint64 currentPlaybackPosition() const;
	[[nodiscard]] bool isPlaybackActive() const;
	void setPlaybackActive(bool active);
	void applyPlaybackSpeed(double speed);
	// Snaps to the nearest offered speed.
	void selectPlaybackSpeed(double speed);
	[[nodiscard]] bool hasAbInterval() const { return _loopStart >= 0 && _loopEnd > _loopStart; }
	void clearAbInterval();
	// Returns the new item's index in the saved-loop combo.
	int addSavedLoopItem(qint64 startMs, qint64 endMs, const QString& name, double speed);
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
	void resolvePendingFormatError();
	void reportFatalPlaybackError(const QString& details);
	void reportRecoverableFormatError(const QString& details);

	void showContextMenu(const QPoint& globalPos);
	void copyFileToClipboard() const;
	void extractFrameToTarget(qint64 timestampMs);
	void extractFrameToFolder(qint64 timestampMs, const QString& folder);
	void repeatLastExtraction(qint64 timestampMs);

	bool eventFilter(QObject* watched, QEvent* event) override;

private:
	static std::vector<VideoPlayerWindow*> _instances;

	const MetadataStoreProvider _metadataStore;
	MediaId _mediaId;
	QString _videoPath;
	QStringList _navigationPaths;
	int _navigationIndex = -1;
	std::function<void(int index)> _onNavigated;
	FrameTarget _frameTarget;
	// Both empty until setRemovalActions.
	RemovalAction _deleteKeyAction;
	RemovalAction _shiftDeleteKeyAction;
	// Seek target for when the file being loaded becomes seekable.
	std::optional<qint64> _positionToRestoreMs;
	// Recorded by releaseFilesMatching for resumeReleasedFile.
	qint64 _releasedPositionMs = 0;
	bool _releasedWasPlaying = false;

	QMediaPlayer* _player = nullptr;
	QAudioOutput* _audioOutput = nullptr;
	QVideoWidget* _videoWidget = nullptr;
	MarkerSlider* _seekSlider = nullptr;
	QLabel* _timeLabel = nullptr;
	QSlider* _volumeSlider = nullptr;
	QComboBox* _speedCombo = nullptr;
	QPushButton* _loopStartButton = nullptr;
	QPushButton* _loopEndButton = nullptr;
	QCheckBox* _oscillationCheck = nullptr;
	QComboBox* _oscillationCurveCombo = nullptr;
	QComboBox* _savedLoopCombo = nullptr;
	// Borrows this window and its video sink; the destructor resets it before QObject child teardown begins.
	std::unique_ptr<OscillatingPlayback> _oscillatingPlayback;
	bool _windowPlacementDone = false;
	bool _pauseOnSeek = true;
	bool _wasPlayingBeforeSeek = false;
	bool _userMuted = false;
	std::optional<QString> _pendingFormatError;
	bool _formatWarningReported = false;
	bool _fatalPlaybackErrorReported = false;

	qint64 _loopStart = -1;
	qint64 _loopEnd = -1;
};
