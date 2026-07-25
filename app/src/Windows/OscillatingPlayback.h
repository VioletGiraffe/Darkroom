#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QProcess>
#include <QSize>
#include <QTimer>

#include <optional>
#include <vector>

class QString;
class VideoPlayerWindow;

enum class OscillationCurve
{
	Linear,
	Cosine,
	TrueCosine,
	Smoothstep,
	Smootherstep,
};

struct OscillationRequest
{
	qint64 startMs = 0;
	qint64 endMs = 0;
	QSize frameSize;
	qreal frameRate = 0;
	int maximumFrameCount = 0;
};

struct OscillationStopResult
{
	std::optional<qint64> displayedPositionMs;
	bool shouldResumePlayback = false;
};

// Owns asynchronous ffmpeg cache preparation, MJPEG parsing, and timer-driven forward/backward presentation.
class OscillatingPlayback final
{
public:
	enum class State
	{
		Inactive,
		Preparing,
		Playing,
		Paused,
	};

	explicit OscillatingPlayback(VideoPlayerWindow& window);
	~OscillatingPlayback();

	[[nodiscard]] State state() const { return _state; }
	[[nodiscard]] bool active() const { return _state != State::Inactive; }
	[[nodiscard]] bool playingOrPending() const { return _state == State::Playing || (_state == State::Preparing && _playWhenReady); }
	[[nodiscard]] std::optional<qint64> displayedPositionMs() const { return _displayedPositionMs; }

	[[nodiscard]] bool start(const OscillationRequest& request, bool playWhenReady, QString* immediateError);
	[[nodiscard]] OscillationStopResult stop();
	void setPlaying(bool playing);
	void setMaximumSpeed(double speed);
	void setCurve(OscillationCurve curve);
	void resetElapsedBaselineAfterGuiBlock();

private:
	enum class MjpegParseState
	{
		Boundary,
		Headers,
		Payload,
		Complete,
	};

	void startPreparationProcess();
	void consumeMjpegParts();
	void finishPreparation(int exitCode, QProcess::ExitStatus exitStatus);
	void fail(const QString& message);
	void advanceAndPresent();
	void presentCurrentPhase();
	[[nodiscard]] bool presentFrame(int frameIndex, QString* error);
	void clearCacheAndParser();

	VideoPlayerWindow& _window;
	State _state = State::Inactive;
	OscillationRequest _request;
	bool _playWhenReady = false;
	double _maximumSpeed = 1.0;
	OscillationCurve _curve = OscillationCurve::Cosine;

	QProcess _process;
	QTimer _presentationTimer;
	QElapsedTimer _elapsedTimer;

	MjpegParseState _parseState = MjpegParseState::Boundary;
	QByteArray _stdoutBuffer;
	QByteArray _stderr;
	qsizetype _expectedPayloadSize = -1;
	std::vector<QByteArray> _compressedFrames;

	double _cyclePhase = 0;
	int _displayedFrameIndex = -1;
	std::optional<qint64> _displayedPositionMs;
	bool _cancellingPreparation = false;
};
