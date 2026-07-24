#include "Windows/VideoPlayerWindow.h"
#include "UiComponents/MarkerSlider.h"
#include "Core/Catalog.h"
#include "Core/Library.h"
#include "Core/MetadataStore.h"
#include "Theme/Icons.h"
#include "Ffmpeg.h"
#include "Import.h"
#include "Settings.h"
#include "Utils.h"
#include "assert/advanced_assert.h"

#include <QAudio>
#include <QAudioOutput>
#include <QByteArray>
#include <QCheckBox>
#include <QComboBox>
#include <QCursor>
#include <QDir>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QImage>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMediaMetaData>
#include <QMediaPlayer>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QScreen>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSlider>
#include <QStringList>
#include <QTemporaryDir>
#include <QTime>
#include <QTimer>
#include <QToolTip>
#include <QUrl>
#include <QVBoxLayout>
#include <QVideoFrame>
#include <QVideoFrameFormat>
#include <QVideoSink>
#include <QVideoWidget>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <optional>
#include <stdint.h>

namespace Settings {
	constexpr const char* PauseOnSeek  = "VideoPlayer/PauseOnSeek";
	constexpr const char* Volume       = "VideoPlayer/Volume"; // UI slider position, 0..100 (perceptual)
	constexpr const char* Muted        = "VideoPlayer/Muted";
	constexpr const char* LastFrameExtractionMode   = "VideoPlayer/LastFrameExtractionMode";   // "library" or "folder"; unset = no extraction done yet
	constexpr const char* LastFrameExtractionFolder = "VideoPlayer/LastFrameExtractionFolder";
}

namespace Defaults {
	constexpr bool PauseOnSeek = true;
	constexpr int  Volume      = 100; // full, on the 0..100 UI scale
	constexpr bool Muted       = false;
}

namespace {
// Item-data roles under which a saved-loop combo entry stores its interval endpoints (ms), optional name, and
// playback speed (0 for legacy loops saved before the speed attribute existed).
enum LoopItemDataRole { LoopStartRole = Qt::UserRole, LoopEndRole = Qt::UserRole + 1, LoopNameRole = Qt::UserRole + 2, LoopSpeedRole = Qt::UserRole + 3 };

// A saved loop's dropdown label: optional name first, then the start as a clock time and the duration as
// fractional seconds to 1 digit, then the playback speed when it isn't 1× (e.g. "warmup   0:12 + 2.3s @2×").
QString formatLoopLabel(qint64 startMs, qint64 endMs, const QString& name, double speed)
{
	const QString start = QTime::fromMSecsSinceStartOfDay(static_cast<int>(startMs)).toString(startMs >= 3600000 ? "h:mm:ss" : "m:ss");
	QString label = start + " + " + QString::number((endMs - startMs) / 1000.0, 'f', 1) + "s";
	if (speed > 0 && qAbs(speed - 1.0) > 0.001)
		label += " @" + QString::number(speed) + "×";
	return name.isEmpty() ? label : name + "   " + label;
}

// Settings::LastFrameExtractionMode values.
constexpr const char* ExtractToLibraryMode = "library";
constexpr const char* ExtractToFolderMode  = "folder";

// Volume-slider units (0..100) changed per wheel notch over the video.
constexpr int VolumeWheelStep = 5;

constexpr qint64 MaxOscillationDurationMs = 30000;
constexpr qreal MaxOscillationFrameRate = 60.0;
constexpr QSize MaxOscillationFrameSize{ 1920, 1080 };
constexpr int OscillationTimerIntervalMs = 16;
constexpr int OscillationJpegQuality = 2;
constexpr int ExpectedFrameCountAllowance = 2;
constexpr QImage::Format OscillationFallbackImageFormat = QImage::Format_RGBA8888;
constexpr const char* OscillationMjpegBoundary = "darkroom_oscillation";
constexpr const char* OscillationCurveSettingKey = "VideoPlayer/OscillationCurve";
constexpr const char* DefaultOscillationCurveSetting = "cosine";
// A nonzero linear share keeps eased curves moving through the turnaround instead of visually dwelling there.
constexpr double CosineLinearShare = 0.6;
constexpr double SmoothstepLinearShare = 0.5;
constexpr double SmootherstepLinearShare = 0.5;

enum class OscillationCurve
{
	Linear,
	Cosine,
	TrueCosine,
	Smoothstep,
	Smootherstep,
};

OscillationCurve oscillationCurveFromSetting(const QString& value)
{
	if (value == "linear")
		return OscillationCurve::Linear;
	if (value == "true_cosine")
		return OscillationCurve::TrueCosine;
	if (value == "smoothstep")
		return OscillationCurve::Smoothstep;
	if (value == "smootherstep")
		return OscillationCurve::Smootherstep;
	return OscillationCurve::Cosine;
}

QString oscillationCurveSetting(OscillationCurve curve)
{
	switch (curve)
	{
	case OscillationCurve::Linear:
		return "linear";
	case OscillationCurve::Cosine:
		return "cosine";
	case OscillationCurve::TrueCosine:
		return "true_cosine";
	case OscillationCurve::Smoothstep:
		return "smoothstep";
	case OscillationCurve::Smootherstep:
		return "smootherstep";
	}
	assert_r(false);
	return "cosine";
}

double oscillationCurvePosition(OscillationCurve curve, double phase)
{
	double position = phase;
	switch (curve)
	{
	case OscillationCurve::Linear:
		break;
	case OscillationCurve::Cosine:
		position = CosineLinearShare * phase
			+ (1.0 - CosineLinearShare) * (1.0 - std::cos(std::numbers::pi * phase)) / 2.0;
		break;
	case OscillationCurve::TrueCosine:
		position = (1.0 - std::cos(std::numbers::pi * phase)) / 2.0;
		break;
	case OscillationCurve::Smoothstep:
		position = SmoothstepLinearShare * phase
			+ (1.0 - SmoothstepLinearShare) * phase * phase * (3.0 - 2.0 * phase);
		break;
	case OscillationCurve::Smootherstep:
		position = SmootherstepLinearShare * phase
			+ (1.0 - SmootherstepLinearShare) * phase * phase * phase * (phase * (phase * 6.0 - 15.0) + 10.0);
		break;
	}
	return std::clamp(position, 0.0, 1.0);
}

double oscillationCurvePeakSlope(OscillationCurve curve)
{
	switch (curve)
	{
	case OscillationCurve::Linear:
		return 1.0;
	case OscillationCurve::Cosine:
		return CosineLinearShare + (1.0 - CosineLinearShare) * std::numbers::pi / 2.0;
	case OscillationCurve::TrueCosine:
		return std::numbers::pi / 2.0;
	case OscillationCurve::Smoothstep:
		return SmoothstepLinearShare + (1.0 - SmoothstepLinearShare) * 1.5;
	case OscillationCurve::Smootherstep:
		return SmootherstepLinearShare + (1.0 - SmootherstepLinearShare) * 1.875;
	}
	assert_r(false);
	return 1.0;
}

struct OscillationStopResult
{
	std::optional<qint64> displayedPositionMs;
	bool shouldResumePlayback = false;
};
}

struct VideoPlayerWindow::OscillationRequest
{
	qint64 startMs = 0;
	qint64 endMs = 0;
	QSize frameSize;
	qreal frameRate = 0;
	int maximumFrameCount = 0;
};

std::vector<VideoPlayerWindow*> VideoPlayerWindow::_instances;


class VideoPlayerWindow::OscillatingPlayback
{
public:
	enum class State
	{
		Inactive,
		Preparing,
		Playing,
		Paused,
	};

	explicit OscillatingPlayback(VideoPlayerWindow& window)
		: _window(window)
	{
		_presentationTimer.setTimerType(Qt::PreciseTimer);
		_presentationTimer.setInterval(OscillationTimerIntervalMs);

		QObject::connect(&_presentationTimer, &QTimer::timeout, &_window, [this] { advanceAndPresent(); });
		QObject::connect(&_process, &QProcess::readyReadStandardOutput, &_window, [this] {
			if (_cancellingPreparation)
				return;
			_stdoutBuffer += _process.readAllStandardOutput();
			consumeMjpegParts();
		});
		QObject::connect(&_process, &QProcess::readyReadStandardError, &_window, [this] {
			if (!_cancellingPreparation)
				_stderr += _process.readAllStandardError();
		});
		QObject::connect(&_process, &QProcess::finished, &_window, [this](int exitCode, QProcess::ExitStatus exitStatus) {
			finishPreparation(exitCode, exitStatus);
		});
		QObject::connect(&_process, &QProcess::errorOccurred, &_window, [this](QProcess::ProcessError error) {
			if (!_cancellingPreparation && _state == State::Preparing && error == QProcess::FailedToStart)
				fail(_window.tr("Could not start ffmpeg: %1").arg(_process.errorString()));
		});
	}

	~OscillatingPlayback()
	{
		if (active())
			(void)stop();
	}

	[[nodiscard]] State state() const { return _state; }
	[[nodiscard]] bool active() const { return _state != State::Inactive; }
	[[nodiscard]] bool playingOrPending() const { return _state == State::Playing || (_state == State::Preparing && _playWhenReady); }
	[[nodiscard]] std::optional<qint64> displayedPositionMs() const { return _displayedPositionMs; }

	[[nodiscard]] bool start(const OscillationRequest& request, bool playWhenReady, QString* immediateError)
	{
		if (_state != State::Inactive)
		{
			assert_r(false);
			if (immediateError)
				*immediateError = _window.tr("Oscillating playback is already active.");
			return false;
		}

		clearCacheAndParser();
		_request = request;
		_playWhenReady = playWhenReady;
		_cyclePhase = 0;
		_displayedFrameIndex = -1;
		_displayedPositionMs.reset();
		_state = State::Preparing;
		startPreparationProcess();
		return true;
	}

	[[nodiscard]] OscillationStopResult stop()
	{
		const OscillationStopResult result{ _displayedPositionMs, playingOrPending() };
		_presentationTimer.stop();

		if (_state == State::Preparing && _process.state() != QProcess::NotRunning)
		{
			_cancellingPreparation = true;
			_process.kill();
			_process.waitForFinished();
			_cancellingPreparation = false;
		}
		(void)_process.readAllStandardOutput();
		(void)_process.readAllStandardError();

		_state = State::Inactive;
		clearCacheAndParser();
		return result;
	}

	void setPlaying(bool playing)
	{
		if (_state == State::Preparing)
		{
			_playWhenReady = playing;
			return;
		}
		if (_state == State::Playing && !playing)
		{
			_presentationTimer.stop();
			_state = State::Paused;
			return;
		}
		if (_state == State::Paused && playing)
		{
			_state = State::Playing;
			_elapsedTimer.restart();
			_presentationTimer.start();
		}
	}

	void setMaximumSpeed(double speed)
	{
		if (std::isfinite(speed) && speed > 0)
			_maximumSpeed = speed;
	}

	void setCurve(OscillationCurve curve)
	{
		_curve = curve;
	}

	void resetElapsedBaselineAfterGuiBlock()
	{
		if (_state == State::Playing)
			_elapsedTimer.restart();
	}

private:
	enum class MjpegParseState
	{
		Boundary,
		Headers,
		Payload,
		Complete,
	};

	void startPreparationProcess()
	{
		const qreal durationSeconds = (_request.endMs - _request.startMs) / qreal(1000);
		const QString filter = QString("fps=%1:start_time=0,scale=%2:%3,setsar=1")
			.arg(QString::number(_request.frameRate, 'f', 6))
			.arg(_request.frameSize.width())
			.arg(_request.frameSize.height());
		const QStringList arguments{
			"-nostdin",
			"-hide_banner",
			"-loglevel", "error",
			"-ss", QString::number(_request.startMs / qreal(1000), 'f', 3),
			"-i", QDir::toNativeSeparators(_window._videoPath),
			"-map", "0:v:0",
			"-an",
			"-sn",
			"-dn",
			"-t", QString::number(durationSeconds, 'f', 3),
			"-vf", filter,
			"-frames:v", QString::number(_request.maximumFrameCount),
			"-c:v", "mjpeg",
			"-q:v", QString::number(OscillationJpegQuality),
			"-f", "mpjpeg",
			"-boundary_tag", OscillationMjpegBoundary,
			"pipe:1",
		};

		_process.setProcessChannelMode(QProcess::SeparateChannels);
		_process.start(ffmpegPath(), arguments);
	}

	void consumeMjpegParts()
	{
		const QByteArray boundary = QByteArray("--") + OscillationMjpegBoundary;
		while (_state == State::Preparing)
		{
			switch (_parseState)
			{
			case MjpegParseState::Boundary:
			{
				const qsizetype lineEnd = _stdoutBuffer.indexOf("\r\n");
				if (lineEnd < 0)
					return;
				const QByteArray line = _stdoutBuffer.left(lineEnd);
				_stdoutBuffer.remove(0, lineEnd + 2);
				if (line == boundary)
					_parseState = MjpegParseState::Headers;
				else if (line == boundary + "--")
					_parseState = MjpegParseState::Complete;
				else
				{
					fail(_window.tr("ffmpeg returned an invalid oscillation-cache boundary."));
					return;
				}
				break;
			}
			case MjpegParseState::Headers:
			{
				const qsizetype headerEnd = _stdoutBuffer.indexOf("\r\n\r\n");
				if (headerEnd < 0)
					return;
				const QByteArray headerBlock = _stdoutBuffer.left(headerEnd);
				_stdoutBuffer.remove(0, headerEnd + 4);

				bool jpegContentType = false;
				int contentLengthCount = 0;
				qint64 contentLength = -1;
				for (QByteArray line : headerBlock.split('\n'))
				{
					line = line.trimmed();
					const qsizetype colon = line.indexOf(':');
					if (colon <= 0)
						continue;
					const QByteArray name = line.left(colon).trimmed().toLower();
					const QByteArray value = line.mid(colon + 1).trimmed();
					if (name == "content-type")
						jpegContentType = value.compare("image/jpeg", Qt::CaseInsensitive) == 0;
					else if (name == "content-length")
					{
						bool ok = false;
						contentLength = value.toLongLong(&ok, 10);
						if (!ok)
							contentLength = -1;
						++contentLengthCount;
					}
				}

				const qint64 maximumPayload = std::numeric_limits<qsizetype>::max() - 2;
				if (!jpegContentType || contentLengthCount != 1 || contentLength <= 0 || contentLength > maximumPayload)
				{
					fail(_window.tr("ffmpeg returned invalid oscillation-cache headers."));
					return;
				}
				_expectedPayloadSize = static_cast<qsizetype>(contentLength);
				_parseState = MjpegParseState::Payload;
				break;
			}
			case MjpegParseState::Payload:
				if (_stdoutBuffer.size() < _expectedPayloadSize + 2)
					return;
				if (_stdoutBuffer.mid(_expectedPayloadSize, 2) != "\r\n")
				{
					fail(_window.tr("ffmpeg returned a malformed oscillation-cache frame."));
					return;
				}
				_compressedFrames.push_back(_stdoutBuffer.left(_expectedPayloadSize));
				_stdoutBuffer.remove(0, _expectedPayloadSize + 2);
				_expectedPayloadSize = -1;
				_parseState = MjpegParseState::Boundary;
				if (_compressedFrames.size() > static_cast<size_t>(_request.maximumFrameCount))
				{
					fail(_window.tr("ffmpeg produced too many oscillation-cache frames."));
					return;
				}
				break;
			case MjpegParseState::Complete:
				if (!_stdoutBuffer.trimmed().isEmpty())
					fail(_window.tr("ffmpeg returned data after the oscillation cache ended."));
				return;
			}
		}
	}

	void finishPreparation(int exitCode, QProcess::ExitStatus exitStatus)
	{
		if (_cancellingPreparation || _state != State::Preparing)
			return;

		_stdoutBuffer += _process.readAllStandardOutput();
		_stderr += _process.readAllStandardError();
		consumeMjpegParts();
		if (_state != State::Preparing)
			return;

		if (exitStatus != QProcess::NormalExit || exitCode != 0)
		{
			fail(_window.tr("ffmpeg could not prepare the oscillation cache."));
			return;
		}

		const bool parserComplete = (_parseState == MjpegParseState::Headers && _stdoutBuffer.isEmpty())
			|| (_parseState == MjpegParseState::Complete && _stdoutBuffer.trimmed().isEmpty());
		if (!parserComplete || _expectedPayloadSize >= 0)
		{
			fail(_window.tr("ffmpeg returned a truncated oscillation cache."));
			return;
		}
		if (_compressedFrames.size() < 2 || _compressedFrames.size() > static_cast<size_t>(_request.maximumFrameCount))
		{
			fail(_window.tr("ffmpeg did not produce enough oscillation-cache frames."));
			return;
		}

		QString error;
		if (!presentFrame(0, &error))
		{
			fail(error);
			return;
		}

		_state = _playWhenReady ? State::Playing : State::Paused;
		if (_state == State::Playing)
		{
			_elapsedTimer.start();
			_presentationTimer.start();
		}
		_window.onOscillationPrepared();
	}

	void fail(const QString& message)
	{
		if (_state == State::Inactive || _cancellingPreparation)
			return;

		const bool shouldResumePlayback = playingOrPending();
		const bool hasDisplayedPosition = _displayedPositionMs.has_value();
		const qint64 displayedPosition = _displayedPositionMs.value_or(0);

		_presentationTimer.stop();
		_cancellingPreparation = true;
		if (_process.state() != QProcess::NotRunning)
		{
			_process.kill();
			_process.waitForFinished();
		}
		(void)_process.readAllStandardOutput();
		_stderr += _process.readAllStandardError();
		QString completeMessage = message;
		const QString diagnostics = QString::fromUtf8(_stderr).trimmed();
		if (!diagnostics.isEmpty() && !completeMessage.contains(diagnostics))
			completeMessage += "\n\n" + diagnostics;
		_state = State::Inactive;
		clearCacheAndParser();
		_cancellingPreparation = false;
		_window.onOscillationFailed(completeMessage, displayedPosition, hasDisplayedPosition, shouldResumePlayback);
	}

	void advanceAndPresent()
	{
		if (_state != State::Playing)
			return;

		const double intervalSeconds = (_request.endMs - _request.startMs) / 1000.0;
		const double phaseRate = _maximumSpeed / (intervalSeconds * oscillationCurvePeakSlope(_curve));
		_cyclePhase = std::fmod(_cyclePhase + _elapsedTimer.restart() / 1000.0 * phaseRate, 2.0);
		if (_cyclePhase < 0)
			_cyclePhase += 2.0;
		presentCurrentPhase();
	}

	void presentCurrentPhase()
	{
		const double legPhase = _cyclePhase <= 1.0 ? _cyclePhase : 2.0 - _cyclePhase;
		const double offsetMs = oscillationCurvePosition(_curve, legPhase) * (_request.endMs - _request.startMs);
		const qint64 candidateIndex = std::llround(offsetMs * _request.frameRate / 1000.0);
		const int frameIndex = static_cast<int>(std::clamp<qint64>(candidateIndex, 0, static_cast<qint64>(_compressedFrames.size() - 1)));
		if (frameIndex == _displayedFrameIndex)
			return;

		QString error;
		if (!presentFrame(frameIndex, &error))
			fail(error);
	}

	[[nodiscard]] bool presentFrame(int frameIndex, QString* error)
	{
		QImage image = QImage::fromData(_compressedFrames[frameIndex], "JPG");
		if (image.isNull())
		{
			*error = _window.tr("A cached oscillation frame could not be decoded.");
			return false;
		}
		if (image.size() != _request.frameSize)
		{
			*error = _window.tr("A cached oscillation frame has an unexpected size.");
			return false;
		}

		QVideoFrameFormat::PixelFormat pixelFormat = QVideoFrameFormat::pixelFormatFromImageFormat(image.format());
		if (pixelFormat == QVideoFrameFormat::Format_Invalid)
		{
			image = image.convertToFormat(OscillationFallbackImageFormat);
			if (image.isNull())
			{
				*error = _window.tr("A cached oscillation frame could not be converted for display.");
				return false;
			}
			pixelFormat = QVideoFrameFormat::pixelFormatFromImageFormat(image.format());
			assert_r(pixelFormat != QVideoFrameFormat::Format_Invalid);
			if (pixelFormat == QVideoFrameFormat::Format_Invalid)
			{
				*error = _window.tr("The cached oscillation frame format is not supported for display.");
				return false;
			}
		}

		const QVideoFrame videoFrame{ image };
		if (!videoFrame.isValid())
		{
			*error = _window.tr("A cached oscillation frame could not be submitted for display.");
			return false;
		}
		_window._videoWidget->videoSink()->setVideoFrame(videoFrame);

		_displayedFrameIndex = frameIndex;
		_displayedPositionMs = std::clamp(
			_request.startMs + std::llround(frameIndex * 1000.0 / _request.frameRate),
			_request.startMs,
			_request.endMs - 1);
		_window.onOscillationPositionChanged(*_displayedPositionMs);
		return true;
	}

	void clearCacheAndParser()
	{
		_stdoutBuffer.clear();
		_stderr.clear();
		_compressedFrames.clear();
		_parseState = MjpegParseState::Boundary;
		_expectedPayloadSize = -1;
		_displayedFrameIndex = -1;
		_displayedPositionMs.reset();
		_cyclePhase = 0;
	}

private:
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


VideoPlayerWindow::VideoPlayerWindow(Library& library, const QString& videoPath, const MediaId& mediaId, QWidget* parent)
	: QMainWindow(parent), _library(library), _mediaId(mediaId), _videoPath(videoPath)
{
	setAttribute(Qt::WA_DeleteOnClose);
	setWindowTitle(QFileInfo{ videoPath }.completeBaseName());

	const QSettings settings;

	_instances.push_back(this);
	// Initialize Player & Output
	_videoWidget = new QVideoWidget(this);
	_player = new QMediaPlayer(this);
	_audioOutput = new QAudioOutput(this);
	_player->setVideoOutput(_videoWidget);
	_player->setAudioOutput(_audioOutput); // Qt6 QMediaPlayer is silent until an audio sink is attached
	_player->setSource(QUrl::fromLocalFile(videoPath));
	_oscillatingPlayback = std::make_unique<OscillatingPlayback>(*this);

	_videoWidget->installEventFilter(this);

	// Build Layout Structure
	QWidget* centralWidget = new QWidget(this);
	QVBoxLayout* mainLayout = new QVBoxLayout(centralWidget);
	mainLayout->setContentsMargins(4, 0, 4, 4);
	mainLayout->setSpacing(0);
	mainLayout->addWidget(_videoWidget, 1);

	// Controls layout (Slider + Text)
	QHBoxLayout* controlsLayout = new QHBoxLayout();
	controlsLayout->setSpacing(6);
	_seekSlider = new MarkerSlider(Qt::Horizontal, this);
	_timeLabel = new QLabel(tr("video is loading..."), this);

	static constexpr double speeds[] { 0.25, 0.35, 0.5, 0.6, 0.8, 1.0, 1.5, 2.0, 2.5, 3.0, 4.0, 6.0, 8.0, 10.0 };
	QComboBox* speedCombo = new QComboBox(this);
	speedCombo->setToolTip(tr("Playback speed. During oscillation this is the approximate maximum speed."));
	for (const auto& s : speeds)
		speedCombo->addItem(QString::number(s) + "×", s);

	_pauseOnSeek = settings.value(Settings::PauseOnSeek, Defaults::PauseOnSeek).toBool();

	// Applies a speed to the player and oscillation, without persisting. Speed is remembered per video, so only
	// an explicit user pick (the combo handler below) writes it; restore and loop activation apply without saving.
	const auto applySpeed = [this](double speed) {
		_player->setPlaybackRate(speed);
		_oscillatingPlayback->setMaximumSpeed(speed);
	};
	connect(speedCombo, &QComboBox::currentIndexChanged, this, [this, speedCombo, applySpeed](int index) {
		if (index < 0)
			return;
		const double speed = speedCombo->itemData(index).toDouble();
		applySpeed(speed);
		// Remember this video's speed, but store nothing for the 1× default - drop any override so untouched
		// (and reverted) videos carry no metadata at all.
		auto writer = _library.metadataStore().beginBatch();
		if (qAbs(speed - 1.0) < 0.001)
			writer.removeField(_mediaId, u"playbackSpeed");
		else
			writer.set(_mediaId, u"playbackSpeed", speed);
	});

	// Programmatically selects the combo entry nearest to a target speed (for restore and loop activation) and
	// applies it, blocking the change signal so the apply happens exactly once (and does not persist) even when
	// the index is unchanged. A non-positive target is "unset" and left alone, so legacy saved loops (no stored
	// speed) keep the current one.
	const auto selectSpeed = [speedCombo, applySpeed](double speed) {
		if (!(speed > 0))
			return;
		int nearest = -1;
		double nearestDiff = std::numeric_limits<double>::max();
		for (int i = 0; i < speedCombo->count(); ++i)
		{
			const double diff = qAbs(speedCombo->itemData(i).toDouble() - speed);
			if (diff < nearestDiff)
			{
				nearestDiff = diff;
				nearest = i;
			}
		}
		if (nearest < 0)
			return;
		const QSignalBlocker blocker{ speedCombo };
		speedCombo->setCurrentIndex(nearest);
		applySpeed(speedCombo->itemData(nearest).toDouble());
	};

	// Restore this video's remembered speed (absent/≤0 means never customized → default 1.0×). Passing 1.0
	// explicitly (rather than letting selectSpeed no-op on ≤0) keeps the combo and player in sync at 1.0×.
	{
		const double storedSpeed = _library.metadataStore().get(_mediaId, u"playbackSpeed").toDouble();
		selectSpeed(storedSpeed > 0 ? storedSpeed : 1.0);
	}

	auto* pauseOnSeekCheck = new QCheckBox(tr("Pause on seek"), this);
	pauseOnSeekCheck->setChecked(_pauseOnSeek);

	connect(pauseOnSeekCheck, &QCheckBox::toggled, this, [this](bool checked) {
		_pauseOnSeek = checked;
		QSettings{}.setValue(Settings::PauseOnSeek, checked);
	});

	// Volume: the slider is a perceptual 0..100 position; the audio device wants a linear gain, so map through
	// QAudio::convertVolume. Mute is an independent flag on the sink and leaves the volume position untouched.
	_volumeSlider = new QSlider(Qt::Horizontal, this);
	_volumeSlider->setRange(0, 100);
	_volumeSlider->setFixedWidth(90);
	_volumeSlider->setToolTip(tr("Volume"));

	auto* muteButton = new QPushButton(this);
	muteButton->setCheckable(true);
	muteButton->setToolTip(tr("Mute (M). Oscillating playback is always muted."));

	const auto applyVolume = [this](int position) {
		_audioOutput->setVolume(QAudio::convertVolume(position / qreal(100), QAudio::LogarithmicVolumeScale, QAudio::LinearVolumeScale));
	};
	const auto updateMuteIcon = [muteButton] {
		muteButton->setIcon(Theme::tintedIcon(muteButton->isChecked() ? ":/UI/icon_volume_muted.svg" : ":/UI/icon_volume.svg",
		                    &Theme::ThemeColors::TextPrimary));
	};

	// Restore persisted volume/mute directly; the handlers below are wired afterwards, so this doesn't re-persist.
	{
		const int savedVolume = settings.value(Settings::Volume, Defaults::Volume).toInt();
		_userMuted = settings.value(Settings::Muted, Defaults::Muted).toBool();
		_volumeSlider->setValue(savedVolume);
		applyVolume(savedVolume);
		muteButton->setChecked(_userMuted);
		applyEffectiveMute();
		updateMuteIcon();
	}

	connect(_volumeSlider, &QAbstractSlider::valueChanged, this, [applyVolume](int position) {
		applyVolume(position);
		QSettings{}.setValue(Settings::Volume, position);
	});
	connect(muteButton, &QPushButton::toggled, this, [this, updateMuteIcon](bool muted) {
		_userMuted = muted;
		applyEffectiveMute();
		updateMuteIcon();
		QSettings{}.setValue(Settings::Muted, muted);
	});

	// Loop controls. A/B/Clear define the current ("live") loop; the combo holds this video's saved loops
	// (persisted in MetadataStore), and selecting one makes it the active loop.
	auto* setLoopStartButton = new QPushButton("A", this);
	auto* setLoopEndButton = new QPushButton("B", this);
	auto* clearLoopButton = new QPushButton(tr("Clear"), this);
	setLoopStartButton->setCheckable(true);
	setLoopEndButton->setCheckable(true);
	setLoopStartButton->setToolTip(tr("Set loop start at the current position"));
	setLoopEndButton->setToolTip(tr("Set loop end at the current position"));

	auto* loopCombo = new QComboBox(this);
	loopCombo->setToolTip(tr("Saved loops for this video"));
	loopCombo->addItem(tr("No loop")); // index 0: placeholder for "no saved loop selected"
	auto* saveLoopButton = new QPushButton(tr("Save"), this);
	auto* renameLoopButton = new QPushButton(tr("Rename"), this);
	auto* deleteLoopButton = new QPushButton(tr("Delete"), this);
	saveLoopButton->setToolTip(tr("Save the current loop for this video"));
	renameLoopButton->setToolTip(tr("Rename the selected saved loop"));
	deleteLoopButton->setToolTip(tr("Delete the selected saved loop"));

	_oscillationCheck = new QCheckBox(tr("Oscillate"), this);
	_oscillationCurveCombo = new QComboBox(this);
	_oscillationCurveCombo->setToolTip(tr("Motion curve for oscillating playback"));
	_oscillationCurveCombo->addItem(tr("Linear"), "linear");
	_oscillationCurveCombo->addItem(tr("Cosine"), "cosine");
	_oscillationCurveCombo->addItem(tr("True cosine"), "true_cosine");
	_oscillationCurveCombo->addItem(tr("Smooth"), "smoothstep");
	_oscillationCurveCombo->addItem(tr("Extra smooth"), "smootherstep");
	const OscillationCurve savedCurve = oscillationCurveFromSetting(settings.value(
		OscillationCurveSettingKey, DefaultOscillationCurveSetting).toString());
	const QString savedCurveSetting = oscillationCurveSetting(savedCurve);
	_oscillationCurveCombo->setCurrentIndex(_oscillationCurveCombo->findData(savedCurveSetting));
	_oscillatingPlayback->setCurve(savedCurve);

	// Shared loop-control helpers.
	const auto activateLoop = [this, setLoopStartButton, setLoopEndButton, selectSpeed](qint64 start, qint64 end, double speed) {
		exitOscillatingPlayback();
		_loopStart = start;
		_loopEnd = end;
		_seekSlider->setMarkerA(static_cast<int>(start));
		_seekSlider->setMarkerB(static_cast<int>(end));
		setLoopStartButton->setChecked(true);
		setLoopEndButton->setChecked(true);
		selectSpeed(speed);  // a stored non-1× loop speed becomes the active speed; legacy loops (0) keep the current one
		_player->setPosition(start);  // rewind so looping starts now, rather than after the playhead drifts past the end
		updateOscillationAvailability();
	};
	const auto clearLoop = [this, setLoopStartButton, setLoopEndButton] {
		exitOscillatingPlayback();
		_loopStart = _loopEnd = -1;
		_seekSlider->clearMarkers();
		setLoopStartButton->setChecked(false);
		setLoopEndButton->setChecked(false);
		updateOscillationAvailability();
	};
	const auto addIntervalItem = [loopCombo](qint64 start, qint64 end, const QString& name, double speed) {
		loopCombo->addItem(formatLoopLabel(start, end, name, speed));
		const int index = loopCombo->count() - 1;
		loopCombo->setItemData(index, start, LoopStartRole);
		loopCombo->setItemData(index, end, LoopEndRole);
		loopCombo->setItemData(index, name, LoopNameRole);
		loopCombo->setItemData(index, speed, LoopSpeedRole);
		return index;
	};
	const auto persistIntervals = [this, loopCombo] {
		QJsonArray array;
		for (int i = 1; i < loopCombo->count(); ++i) // skip index 0 ("No loop")
		{
			QJsonObject object;
			object.insert("start", loopCombo->itemData(i, LoopStartRole).toLongLong());
			object.insert("end", loopCombo->itemData(i, LoopEndRole).toLongLong());
			object.insert("name", loopCombo->itemData(i, LoopNameRole).toString());
			object.insert("speed", loopCombo->itemData(i, LoopSpeedRole).toDouble());
			array.append(object);
		}
		_library.metadataStore().beginBatch().set(_mediaId, u"intervals", array);  // single write; the temporary Writer flushes right here
	};
	// Optional-name prompt shared by save-new and rename; nullopt only when cancelled (empty string = unnamed).
	const auto promptLoopName = [this](const QString& title, const QString& initial) -> std::optional<QString> {
		bool ok = false;
		const QString name = QInputDialog::getText(this, title, tr("Loop name (optional):"), QLineEdit::Normal, initial, &ok).trimmed();
		if (!ok)
			return std::nullopt;
		return name;
	};

	// Load this video's saved loops into the combo without firing the activation handler wired below.
	{
		const QSignalBlocker blocker{ loopCombo };
		const QJsonArray saved = _library.metadataStore().get(_mediaId, u"intervals").toArray();
		for (const QJsonValue& value : saved)
		{
			const QJsonObject object = value.toObject();
			addIntervalItem(object.value("start").toInteger(), object.value("end").toInteger(),
			                object.value("name").toString(), object.value("speed").toDouble());
		}
	}

	connect(setLoopStartButton, &QPushButton::clicked, this, [this, setLoopStartButton] {
		const qint64 position = currentPlaybackPosition();
		exitOscillatingPlayback();
		_loopStart = position;
		_seekSlider->setMarkerA(static_cast<int>(_loopStart));
		setLoopStartButton->setChecked(true);
		updateOscillationAvailability();
	});
	connect(setLoopEndButton, &QPushButton::clicked, this, [this, setLoopEndButton] {
		const qint64 position = currentPlaybackPosition();
		exitOscillatingPlayback();
		_loopEnd = position;
		_seekSlider->setMarkerB(static_cast<int>(_loopEnd));
		setLoopEndButton->setChecked(true);
		updateOscillationAvailability();
	});
	connect(clearLoopButton, &QPushButton::clicked, this, [loopCombo, clearLoop] {
		clearLoop();
		const QSignalBlocker blocker{ loopCombo };
		loopCombo->setCurrentIndex(0); // deselect any saved loop without re-clearing via the handler
	});

	// Selecting a saved loop activates it; selecting "No loop" (index 0) clears the live loop.
	connect(loopCombo, &QComboBox::currentIndexChanged, this, [loopCombo, activateLoop, clearLoop](int index) {
		if (index <= 0)
		{
			clearLoop();
			return;
		}
		activateLoop(loopCombo->itemData(index, LoopStartRole).toLongLong(),
		             loopCombo->itemData(index, LoopEndRole).toLongLong(),
		             loopCombo->itemData(index, LoopSpeedRole).toDouble());
	});
	connect(saveLoopButton, &QPushButton::clicked, this, [this, loopCombo, speedCombo, addIntervalItem, persistIntervals, promptLoopName] {
		if (!hasAbInterval())
			return; // nothing valid to save
		const std::optional<QString> name = promptLoopName(tr("Save loop"), {});
		if (!name)
			return; // cancelled
		const int index = addIntervalItem(_loopStart, _loopEnd, *name, speedCombo->currentData().toDouble());
		persistIntervals();
		loopCombo->setCurrentIndex(index); // reflect the saved loop as the active selection
	});
	connect(deleteLoopButton, &QPushButton::clicked, this, [loopCombo, persistIntervals] {
		const int index = loopCombo->currentIndex();
		if (index <= 0)
			return;
		// Block signals across the removal: removing the current item would otherwise auto-select (and
		// activate) a neighbour. The live loop is left as-is; only the saved entry is removed.
		const QSignalBlocker blocker{ loopCombo };
		loopCombo->removeItem(index);
		loopCombo->setCurrentIndex(0);
		persistIntervals();
	});

	connect(renameLoopButton, &QPushButton::clicked, this, [loopCombo, promptLoopName, persistIntervals] {
		const int index = loopCombo->currentIndex();
		if (index <= 0)
			return; // "No loop" selected - nothing to rename
		const std::optional<QString> name = promptLoopName(tr("Rename loop"), loopCombo->itemData(index, LoopNameRole).toString());
		if (!name)
			return; // cancelled
		const qint64 start = loopCombo->itemData(index, LoopStartRole).toLongLong();
		const qint64 end   = loopCombo->itemData(index, LoopEndRole).toLongLong();
		const double speed = loopCombo->itemData(index, LoopSpeedRole).toDouble();
		loopCombo->setItemText(index, formatLoopLabel(start, end, *name, speed));
		loopCombo->setItemData(index, *name, LoopNameRole);
		persistIntervals();
	});

	// Keyboard equivalents for the loop endpoint buttons; route through click() to reuse their handlers.
	QShortcut* setLoopStartShortcut = new QShortcut(QKeySequence(Qt::Key_BracketLeft), this);
	connect(setLoopStartShortcut, &QShortcut::activated, setLoopStartButton, &QPushButton::click);
	QShortcut* setLoopEndShortcut = new QShortcut(QKeySequence(Qt::Key_BracketRight), this);
	connect(setLoopEndShortcut, &QShortcut::activated, setLoopEndButton, &QPushButton::click);

	controlsLayout->addWidget(_seekSlider);
	controlsLayout->addWidget(_timeLabel);
	controlsLayout->addWidget(speedCombo);
	controlsLayout->addWidget(pauseOnSeekCheck);
	controlsLayout->addWidget(muteButton);
	controlsLayout->addWidget(_volumeSlider);

	// Loop controls live on their own row to keep the seek row uncluttered.
	QHBoxLayout* loopLayout = new QHBoxLayout();
	loopLayout->setSpacing(6);
	loopLayout->addWidget(new QLabel(tr("Loop:"), this));
	loopLayout->addWidget(setLoopStartButton);
	loopLayout->addWidget(setLoopEndButton);
	loopLayout->addWidget(clearLoopButton);
	loopLayout->addWidget(_oscillationCheck);
	loopLayout->addWidget(_oscillationCurveCombo);
	loopLayout->addWidget(loopCombo, 1);
	loopLayout->addWidget(saveLoopButton);
	loopLayout->addWidget(renameLoopButton);
	loopLayout->addWidget(deleteLoopButton);

	connect(_oscillationCurveCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
		if (index < 0)
			return;
		const OscillationCurve curve = oscillationCurveFromSetting(_oscillationCurveCombo->itemData(index).toString());
		_oscillatingPlayback->setCurve(curve);
		QSettings{}.setValue(OscillationCurveSettingKey, oscillationCurveSetting(curve));
	});
	connect(_oscillationCheck, &QCheckBox::toggled, this, [this](bool checked) {
		if (checked)
			startOscillatingPlayback();
		else
			exitOscillatingPlayback();
	});

	mainLayout->addLayout(controlsLayout);
	mainLayout->addLayout(loopLayout);

	setCentralWidget(centralWidget);

	// Configure Looping
	_player->setLoops(QMediaPlayer::Infinite);

	// Progress Tracking & Seeking Connections
	connect(_player, &QMediaPlayer::durationChanged, this, [this](qint64 duration) {
		_seekSlider->setRange(0, static_cast<int>(duration));
		updateOscillationAvailability();
	});

	connect(_player, &QMediaPlayer::positionChanged, this, [this](qint64 position) {
		// A-B loop: jump back to the start once playback reaches the end marker.
		// The follow-up positionChanged from the seek refreshes the slider/label below.
		if (!_oscillatingPlayback->active() && hasAbInterval() && position >= _loopEnd)
		{
			_player->setPosition(_loopStart);
			return;
		}
		if (!_oscillatingPlayback->active())
			updatePlaybackPositionUi(position);
	});

	// Mouse drag: always pause on press, resume on release if pause-on-seek is off.
	// Keyboard seeks don't fire sliderPressed/Released, so valueChanged handles them.
	connect(_seekSlider, &QAbstractSlider::sliderPressed, this, [this] {
		_wasPlayingBeforeSeek = isPlaybackActive();
		setPlaybackActive(false);
	});
	connect(_seekSlider, &QAbstractSlider::valueChanged, this, [this](int value) {
		if (_oscillatingPlayback->active())
			exitOscillatingPlayback();
		_player->setPosition(value);
		if (!_seekSlider->isSliderDown() && _pauseOnSeek)
			setPlaybackActive(false);
	});
	connect(_seekSlider, &QAbstractSlider::sliderReleased, this, [this] {
		if (!_pauseOnSeek && _wasPlayingBeforeSeek)
			setPlaybackActive(true);
	});

	// Resize to the video size
	connect(_videoWidget->videoSink(), &QVideoSink::videoSizeChanged, this, [this] {
		resizeAndMoveWindow();
		updateOscillationAvailability();
	});

	// Spacebar Play/Pause Toggle
	QShortcut* spaceShortcut = new QShortcut(QKeySequence(Qt::Key_Space), this);
	connect(spaceShortcut, &QShortcut::activated, this, &VideoPlayerWindow::togglePlayPause);

	// Escape leaves fullscreen if in it, otherwise closes the window.
	QShortcut* closeWindowShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), this);
	connect(closeWindowShortcut, &QShortcut::activated, this, [this] {
		if (isFullScreen())
			showNormal();
		else
			close();
	});

	QShortcut* fullScreenShortcut = new QShortcut(QKeySequence(Qt::Key_F), this);
	connect(fullScreenShortcut, &QShortcut::activated, this, &VideoPlayerWindow::toggleFullScreen);

	// 'M' toggles mute via the button, so its handler (icon + persistence) runs.
	QShortcut* muteShortcut = new QShortcut(QKeySequence(Qt::Key_M), this);
	connect(muteShortcut, &QShortcut::activated, muteButton, &QAbstractButton::toggle);

	// 'E' repeats the last frame extraction at the current position.
	QShortcut* extractFrameShortcut = new QShortcut(QKeySequence(Qt::Key_E), this);
	connect(extractFrameShortcut, &QShortcut::activated, this, [this] { repeatLastExtraction(currentPlaybackPosition()); });

	// Start playback
	updateOscillationAvailability();
	setPlaybackActive(true);
}

VideoPlayerWindow::~VideoPlayerWindow()
{
	_oscillatingPlayback.reset();
	std::erase(_instances, this);
}

void VideoPlayerWindow::restartAll()
{
	for (VideoPlayerWindow* win : _instances)
	{
		win->exitOscillatingPlayback(false);
		win->_player->pause();
		win->_player->setPosition(0);
	}

	// Small delay to ensure all players have reset before starting playback again
	QTimer::singleShot(100, [] {
		for (VideoPlayerWindow* win : _instances)
		{
			win->setPlaybackActive(true);
		}
		});
}

void VideoPlayerWindow::closeAll()
{
	// Destroy synchronously so a root switch cannot return to the event loop with players for the old library.
	const std::vector<VideoPlayerWindow*> instances = _instances;
	for (VideoPlayerWindow* win : instances)
		delete win;
}

void VideoPlayerWindow::createPlayerWindow(Library& library, const QString& videoPath, QWidget* parent)
{
	auto* player = new VideoPlayerWindow(library, videoPath, MediaId::fromFile(videoPath), parent);
	player->show();
}

void VideoPlayerWindow::resizeAndMoveWindow()
{
	if (_windowPlacementDone)
		return;

	const QSize sourceVideoSize = _videoWidget->videoSink()->videoSize();
	if (!sourceVideoSize.isValid())
		return;
	_windowPlacementDone = true;

	QScreen* const targetScreen = screen() ? screen() : QGuiApplication::primaryScreen();
	const QRect available = targetScreen ? targetScreen->availableGeometry() : QRect(0, 0, 1600, 900);

	QSize targetVideoSize = sourceVideoSize;
	if (targetVideoSize.width() > 1280 || targetVideoSize.height() > 720)
		targetVideoSize.scale(QSize(1280, 720), Qt::KeepAspectRatio);
	targetVideoSize = targetVideoSize.expandedTo(QSize(300, 200));

	_videoWidget->setMinimumSize(targetVideoSize);
	adjustSize();

	const QSize frameOverhead(qMax(0, frameSize().width() - _videoWidget->width()),
	                          qMax(0, frameSize().height() - _videoWidget->height()));
	const QSize videoAreaLimit(qMax(1, qMin(1280, available.width() - frameOverhead.width())),
	                           qMax(1, qMin(720, available.height() - frameOverhead.height())));
	if (targetVideoSize.width() > videoAreaLimit.width() || targetVideoSize.height() > videoAreaLimit.height())
	{
		targetVideoSize = sourceVideoSize.scaled(videoAreaLimit, Qt::KeepAspectRatio);
		_videoWidget->setMinimumSize(targetVideoSize);
		adjustSize();
	}
	_videoWidget->setMinimumSize(0, 0);

	if (frameSize().width() > available.width() || frameSize().height() > available.height())
	{
		const QSize decorationSize = frameSize() - size();
		resize(qMax(1, available.width() - decorationSize.width()), qMax(1, available.height() - decorationSize.height()));
	}

	constexpr int preferredZoneOrder[]{ 1, 0, 2 };  // center wins ties, followed by left and right
	QRect bestFrame;
	int64_t leastOverlap = std::numeric_limits<int64_t>::max();
	for (const int zone : preferredZoneOrder)
	{
		QRect candidateFrame(QPoint(), frameSize());
		const int anchorX = available.left() + available.width() * (zone * 2 + 1) / 6;
		candidateFrame.moveCenter(QPoint(anchorX, available.center().y()));
		candidateFrame.moveLeft(qBound(available.left(), candidateFrame.left(),
		                               qMax(available.left(), available.right() - candidateFrame.width() + 1)));
		candidateFrame.moveTop(qBound(available.top(), candidateFrame.top(),
		                              qMax(available.top(), available.bottom() - candidateFrame.height() + 1)));

		int64_t overlapArea = 0;
		for (VideoPlayerWindow* win : _instances)
		{
			if (win == this || !win->isVisible() || win->isMinimized() || win->screen() != targetScreen)
				continue;
			const QRect overlap = candidateFrame.intersected(win->frameGeometry());
			if (!overlap.isEmpty())
				overlapArea += static_cast<int64_t>(overlap.width()) * overlap.height();
		}

		if (overlapArea < leastOverlap)
		{
			leastOverlap = overlapArea;
			bestFrame = candidateFrame;
		}
	}

	move(bestFrame.topLeft());
}

qint64 VideoPlayerWindow::currentPlaybackPosition() const
{
	if (const std::optional<qint64> displayedPosition = _oscillatingPlayback->displayedPositionMs())
		return *displayedPosition;
	return _player->position();
}

bool VideoPlayerWindow::isPlaybackActive() const
{
	if (_oscillatingPlayback->active())
		return _oscillatingPlayback->playingOrPending();
	return _player->playbackState() == QMediaPlayer::PlayingState;
}

void VideoPlayerWindow::setPlaybackActive(bool active)
{
	if (_oscillatingPlayback->active())
	{
		_oscillatingPlayback->setPlaying(active);
		return;
	}

	if (active)
		_player->play();
	else
		_player->pause();
}

bool VideoPlayerWindow::buildOscillationRequest(OscillationRequest* request, QString* error) const
{
	assert_r(request);
	assert_r(error);

	const qint64 duration = _player->duration();
	if (duration <= 0)
	{
		*error = tr("The video duration is not available yet.");
		return false;
	}

	// Without an A-B interval the whole video is oscillated.
	const qint64 startMs = hasAbInterval() ? _loopStart : 0;
	const qint64 endMs = hasAbInterval() ? _loopEnd : duration;
	if (endMs > duration)
	{
		*error = tr("The A-B interval must be within the loaded video.");
		return false;
	}

	const qint64 intervalDuration = endMs - startMs;
	if (intervalDuration > MaxOscillationDurationMs)
	{
		const qint64 maxSeconds = MaxOscillationDurationMs / 1000;
		*error = hasAbInterval()
			? tr("Oscillating playback supports intervals up to %1 seconds.").arg(maxSeconds)
			: tr("The video is longer than %1 seconds; set an A-B interval to oscillate.").arg(maxSeconds);
		return false;
	}

	QSize frameSize = _videoWidget->videoSink()->videoSize();
	if (frameSize.width() <= 0 || frameSize.height() <= 0)
	{
		*error = tr("The video dimensions are not available yet.");
		return false;
	}
	if (frameSize.width() > MaxOscillationFrameSize.width() || frameSize.height() > MaxOscillationFrameSize.height())
		frameSize.scale(MaxOscillationFrameSize, Qt::KeepAspectRatio);
	frameSize.setWidth(frameSize.width() / 2 * 2);
	frameSize.setHeight(frameSize.height() / 2 * 2);
	if (frameSize.width() < 2 || frameSize.height() < 2)
	{
		*error = tr("The video dimensions are too small for oscillating playback.");
		return false;
	}

	bool frameRateOk = false;
	qreal frameRate = _player->metaData().value(QMediaMetaData::VideoFrameRate).toDouble(&frameRateOk);
	if (!frameRateOk || !std::isfinite(frameRate) || frameRate <= 0)
		frameRate = MaxOscillationFrameRate;
	frameRate = std::min(frameRate, MaxOscillationFrameRate);

	const int expectedFrameCount = static_cast<int>(std::ceil(intervalDuration * frameRate / 1000.0));
	if (expectedFrameCount < 2)
	{
		*error = tr("The interval is too short for oscillating playback.");
		return false;
	}

	*request = OscillationRequest{
		.startMs = startMs,
		.endMs = endMs,
		.frameSize = frameSize,
		.frameRate = frameRate,
		.maximumFrameCount = expectedFrameCount + ExpectedFrameCountAllowance,
	};
	error->clear();
	return true;
}

void VideoPlayerWindow::updateOscillationAvailability()
{
	if (!_oscillationCheck)
		return;

	OscillationRequest request;
	QString error;
	const bool available = buildOscillationRequest(&request, &error);
	_oscillationCheck->setEnabled(_oscillationCheck->isChecked() || available);
	_oscillationCheck->setToolTip(available
		? (hasAbInterval()
			? tr("Play the A-B interval forward and backward. Audio is muted while active.")
			: tr("Play the whole video forward and backward. Audio is muted while active."))
		: error);
}

void VideoPlayerWindow::startOscillatingPlayback()
{
	OscillationRequest request;
	QString error;
	if (!buildOscillationRequest(&request, &error))
	{
		const QSignalBlocker blocker{ _oscillationCheck };
		_oscillationCheck->setChecked(false);
		updateOscillationAvailability();
		QToolTip::showText(QCursor::pos(), error, _oscillationCheck);
		return;
	}

	const bool wasPlaying = isPlaybackActive();
	_player->pause();
	if (!_oscillatingPlayback->start(request, wasPlaying, &error))
	{
		const QSignalBlocker blocker{ _oscillationCheck };
		_oscillationCheck->setChecked(false);
		setPlaybackActive(wasPlaying);
		updateOscillationAvailability();
		QToolTip::showText(QCursor::pos(), error, _oscillationCheck);
		return;
	}

	applyEffectiveMute();
	_timeLabel->setText(tr("Preparing oscillation..."));
	updateOscillationAvailability();
}

void VideoPlayerWindow::exitOscillatingPlayback(bool restorePlaybackState)
{
	if (!_oscillatingPlayback || !_oscillatingPlayback->active())
	{
		if (_oscillationCheck)
		{
			const QSignalBlocker blocker{ _oscillationCheck };
			_oscillationCheck->setChecked(false);
			updateOscillationAvailability();
		}
		return;
	}

	const OscillationStopResult result = _oscillatingPlayback->stop();
	{
		const QSignalBlocker blocker{ _oscillationCheck };
		_oscillationCheck->setChecked(false);
	}
	if (result.displayedPositionMs)
		_player->setPosition(*result.displayedPositionMs);
	applyEffectiveMute();
	if (restorePlaybackState)
		setPlaybackActive(result.shouldResumePlayback);
	updatePlaybackPositionUi(result.displayedPositionMs.value_or(_player->position()));
	updateOscillationAvailability();
}

void VideoPlayerWindow::updatePlaybackPositionUi(qint64 position)
{
	if (!_seekSlider->isSliderDown())
	{
		const QSignalBlocker blocker{ _seekSlider };
		_seekSlider->setValue(static_cast<int>(position));
	}

	const qint64 duration = _player->duration();
	const QTime posTime = QTime::fromMSecsSinceStartOfDay(static_cast<int>(position));
	const QTime durTime = QTime::fromMSecsSinceStartOfDay(static_cast<int>(duration));
	const QString format = duration >= 3600000 ? "hh:mm:ss.zzz" : "mm:ss.zzz";
	_timeLabel->setText(QString("%1 / %2").arg(posTime.toString(format), durTime.toString(format)));
}

void VideoPlayerWindow::applyEffectiveMute()
{
	_audioOutput->setMuted(_userMuted || (_oscillatingPlayback && _oscillatingPlayback->active()));
}

void VideoPlayerWindow::onOscillationPrepared()
{
	applyEffectiveMute();
	updatePlaybackPositionUi(currentPlaybackPosition());
}

void VideoPlayerWindow::onOscillationPositionChanged(qint64 position)
{
	updatePlaybackPositionUi(position);
}

void VideoPlayerWindow::onOscillationFailed(
	const QString& error, qint64 displayedPosition, bool hasDisplayedPosition, bool shouldResumePlayback)
{
	{
		const QSignalBlocker blocker{ _oscillationCheck };
		_oscillationCheck->setChecked(false);
	}
	if (hasDisplayedPosition)
		_player->setPosition(displayedPosition);
	applyEffectiveMute();
	setPlaybackActive(shouldResumePlayback);
	updatePlaybackPositionUi(hasDisplayedPosition ? displayedPosition : _player->position());
	updateOscillationAvailability();
	QMessageBox::warning(this, tr("Oscillating playback"), error);
}

void VideoPlayerWindow::togglePlayPause()
{
	setPlaybackActive(!isPlaybackActive());
}

void VideoPlayerWindow::toggleFullScreen()
{
	if (isFullScreen())
		showNormal();
	else
		showFullScreen();
}

bool VideoPlayerWindow::eventFilter(QObject* watched, QEvent* event)
{
	// Left click pauses/plays, right click opens the context menu.
	if (event->type() == QEvent::MouseButtonRelease)
	{
		const auto* mouseEvent = static_cast<const QMouseEvent*>(event);
		if (mouseEvent->button() == Qt::LeftButton)
			togglePlayPause();
		else if (mouseEvent->button() == Qt::RightButton)
			showContextMenu(mouseEvent->globalPosition().toPoint());
		return true; // Consume the event so it doesn't propagate to the widget
	}

	// Wheel over the video adjusts volume through the slider, which owns the apply-and-persist logic.
	if (event->type() == QEvent::Wheel)
	{
		const int notches = static_cast<const QWheelEvent*>(event)->angleDelta().y() / 120;
		if (notches != 0)
			_volumeSlider->setValue(_volumeSlider->value() + notches * VolumeWheelStep);
		return true;
	}

	return QMainWindow::eventFilter(watched, event);
}

void VideoPlayerWindow::showContextMenu(const QPoint& globalPos)
{
	// Captured at click time: playback may keep running while the menu is open, and the frame the user
	// right-clicked is the one they mean.
	const qint64 timestampMs = currentPlaybackPosition();

	QMenu menu(this);
	menu.addAction(tr("Extract frame and import to library"), this, [this, timestampMs] { extractFrameToLibrary(timestampMs); });
	menu.addAction(tr("Extract frame to folder..."), this, [this, timestampMs] {
		const QString folder = QFileDialog::getExistingDirectory(this, tr("Extract frame to folder"),
			QSettings{}.value(Settings::LastFrameExtractionFolder).toString());
		if (!folder.isEmpty())
			extractFrameToFolder(timestampMs, folder);
	});

	// Third item: repeat the last extraction with no picker, its label naming that destination; enabled only
	// once one has run.
	const QString lastMode   = QSettings{}.value(Settings::LastFrameExtractionMode).toString();
	const QString lastFolder = QSettings{}.value(Settings::LastFrameExtractionFolder).toString();
	QString repeatText;
	if (lastMode == ExtractToLibraryMode)
		repeatText = tr("Extract frame → library");
	else if (lastMode == ExtractToFolderMode && !lastFolder.isEmpty())
		repeatText = tr("Extract frame → %1").arg(QDir::toNativeSeparators(lastFolder));

	// "\tE" is a display-only hint in the menu's shortcut column; the actual key is the constructor's QShortcut.
	QAction* repeatAction = menu.addAction((repeatText.isEmpty() ? tr("Extract frame (last used)") : repeatText) + "\tE",
		this, [this, timestampMs] { repeatLastExtraction(timestampMs); });
	repeatAction->setEnabled(!repeatText.isEmpty());

	menu.addSeparator();
	menu.addAction((isFullScreen() ? tr("Exit fullscreen") : tr("Fullscreen")) + "\tF", this, &VideoPlayerWindow::toggleFullScreen);

	menu.exec(globalPos);
}

void VideoPlayerWindow::repeatLastExtraction(qint64 timestampMs)
{
	const QString lastMode = QSettings{}.value(Settings::LastFrameExtractionMode).toString();
	if (lastMode == ExtractToLibraryMode)
		extractFrameToLibrary(timestampMs);
	else if (lastMode == ExtractToFolderMode)
	{
		const QString lastFolder = QSettings{}.value(Settings::LastFrameExtractionFolder).toString();
		if (!lastFolder.isEmpty())
			extractFrameToFolder(timestampMs, lastFolder);
	}
	// No prior extraction (or a folder setting since cleared) - nothing to repeat.
}

QString VideoPlayerWindow::extractFrameInto(qint64 timestampMs, const QString& destinationFolder)
{
	// Format and quality follow the full-split settings, so a player-extracted frame matches split output.
	const bool tiff       = QSettings{}.value(Settings::UseTiff, Defaults::UseTiff).toBool();
	const int jpegQuality = QSettings{}.value(Settings::JpegQuality, Defaults::JpegQuality).toInt();

	// The timestamp in the name makes each extracted instant a distinct file (and MediaId); dots instead of
	// colons because Windows forbids colons in file names.
	const QString timestampText = QTime::fromMSecsSinceStartOfDay(static_cast<int>(timestampMs)).toString("h.mm.ss.zzz");
	const QString filePath = destinationFolder + '/' + QFileInfo{ _videoPath }.completeBaseName() + ' ' + timestampText + (tiff ? ".tif" : ".jpg");

	const Ffmpeg::SplitResult result = Ffmpeg::extractFrame(_videoPath, timestampMs, filePath, jpegQuality);
	_oscillatingPlayback->resetElapsedBaselineAfterGuiBlock();
	if (!result.ok())
	{
		reportFfmpegFailure(this, result, _videoPath, destinationFolder);
		return {};
	}
	return filePath;
}

void VideoPlayerWindow::extractFrameToFolder(qint64 timestampMs, const QString& folder)
{
	const QString filePath = extractFrameInto(timestampMs, folder);
	if (filePath.isEmpty())
		return;

	QSettings settings;
	settings.setValue(Settings::LastFrameExtractionMode, ExtractToFolderMode);
	settings.setValue(Settings::LastFrameExtractionFolder, folder);
	QToolTip::showText(QCursor::pos(), tr("Frame saved:\n%1").arg(QDir::toNativeSeparators(filePath)), this);
}

void VideoPlayerWindow::extractFrameToLibrary(qint64 timestampMs)
{
	Catalog& catalog = _library.catalog();

	const QString labelName = QSettings{}.value(Settings::ExtractedLabelName, Defaults::ExtractedLabelName).toString();
	QString error;
	const LabelId labelId = catalog.createLabel(labelName, {}, &error);
	if (labelId == LabelId::None)
	{
		QMessageBox::critical(this, tr("Error"), tr("Failed to create the \"%1\" label:\n%2").arg(labelName, error));
		return;
	}

	const QString photoFolder = catalog.photoFolderForLabel(labelId);
	if (photoFolder.isEmpty())
	{
		QMessageBox::critical(this, tr("Error"), tr("The \"%1\" label has no usable photo folder.").arg(labelName));
		return;
	}

	// Extract into a temp dir under the library root rather than the system temp, so the Move below is a
	// same-drive rename, never a cross-drive copy.
	QTemporaryDir tempDir(_library.rootFolder() + "/.extract-XXXXXX");
	if (!tempDir.isValid())
	{
		QMessageBox::critical(this, tr("Error"), tr("Failed to create a temporary folder in the library:\n%1").arg(tempDir.errorString()));
		return;
	}

	const QString extractedPath = extractFrameInto(timestampMs, tempDir.path());
	if (extractedPath.isEmpty())
		return;

	const Import::PhotoResult result = Import::importPhoto(catalog, photoFolder, extractedPath, Import::PhotoImportMode::Move);
	if (result.status != Import::PhotoStatus::Success)
	{
		QMessageBox::critical(this, tr("Error"), tr("Failed to import the extracted frame:\n%1").arg(result.errorMessage));
		return;
	}

	QSettings{}.setValue(Settings::LastFrameExtractionMode, ExtractToLibraryMode);
	QToolTip::showText(QCursor::pos(), tr("Frame imported into the library under \"%1\"").arg(labelName), this);
}
