#pragma once

#include "Core/MediaId.h"
#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QMainWindow>
#include <QPointer>
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
class Library;
class OscillatingPlayback;
struct OscillationRequest;

class VideoPlayerWindow final : public QMainWindow
{
public:
	// mediaId keys per-video metadata; derive it from videoPath for ad-hoc playback.
	// A null library leaves out the library-bound features: metadata persistence (speed, saved loops),
	// prev/next navigation, and extract-to-library.
	VideoPlayerWindow(Library* library, const QString& videoPath, const MediaId& mediaId, QWidget* parent);
	~VideoPlayerWindow() override;

	// Browsing order for previous/next navigation; empty (the default) leaves navigation inert.
	void setNavigationOrder(std::vector<MediaId> order);
	// Reports each item navigated to; the initially loaded one does not call it.
	void setOnNavigatedToMediaItem(std::function<void(const MediaId& id)> handler);

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

	// While one is alive, no player holds open a file at or under paths.
	// On destruction a player whose file is gone moves on as after a removal; the others resume where they were.
	class FileRelease final
	{
	public:
		explicit FileRelease(const QStringList& paths);
		~FileRelease();
		FileRelease(const FileRelease&) = delete;
		FileRelease& operator=(const FileRelease&) = delete;

	private:
		struct ReleasedPlayer
		{
			QPointer<VideoPlayerWindow> player;
			qint64 positionMs;
			bool wasPlaying;
		};
		std::vector<ReleasedPlayer> _released;
	};

	// Opens a self-managing ad-hoc player.
	static VideoPlayerWindow* createPlayerWindow(Library* library, const QString& videoPath, QWidget* parent);

private:
	friend class OscillatingPlayback;

	// The values are the step through _navigationOrder.
	enum class Direction { Previous = -1, Next = 1 };

	// Everything per-file lives here; the constructor is the first caller.
	void loadFile(const QString& videoPath, const MediaId& mediaId);
	// Skips items that have left the library or the disk, and stops at the ends.
	[[nodiscard]] std::optional<MediaId> adjacentMediaItem(Direction direction) const;
	void loadAdjacentFile(Direction direction);
	void performRemoval(const RemovalAction& action);
	// Next item, else previous, else closes the window.
	void leaveRemovedItem();
	// Reloads the current file after a FileRelease.
	void resumeReleasedFile(qint64 positionMs, bool wasPlaying);
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
	void extractFrameToLibrary(qint64 timestampMs);
	void extractFrameToFolder(qint64 timestampMs, const QString& folder);
	void repeatLastExtraction(qint64 timestampMs);

	bool eventFilter(QObject* watched, QEvent* event) override;

private:
	static std::vector<VideoPlayerWindow*> _instances;

	Library* _library = nullptr;
	MediaId _mediaId;
	QString _videoPath;
	std::function<void(const MediaId& id)> _onNavigatedToMediaItem;
	// The current position is looked up by _mediaId rather than tracked.
	std::vector<MediaId> _navigationOrder;
	// Both empty until setRemovalActions.
	RemovalAction _deleteKeyAction;
	RemovalAction _shiftDeleteKeyAction;
	// Seek target for when the file being loaded becomes seekable.
	std::optional<qint64> _positionToRestoreMs;

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
