#include "Windows/OscillatingPlayback.h"

#include "Windows/VideoPlayerWindow.h"
#include "Utils.h"

#include "assert/advanced_assert.h"

#include <QDir>
#include <QImage>
#include <QObject>
#include <QStringList>
#include <QVideoFrame>
#include <QVideoFrameFormat>
#include <QVideoSink>
#include <QVideoWidget>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace {

constexpr int OscillationTimerIntervalMs = 16;
constexpr int OscillationJpegQuality = 2;
constexpr QImage::Format OscillationFallbackImageFormat = QImage::Format_RGBA8888;
constexpr const char* OscillationMjpegBoundary = "darkroom_oscillation";

// A nonzero linear share keeps eased curves moving through the turnaround instead of visually dwelling there.
constexpr double CosineLinearShare = 0.6;
constexpr double SmoothstepLinearShare = 0.5;
constexpr double SmootherstepLinearShare = 0.5;

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

} // namespace

OscillatingPlayback::OscillatingPlayback(VideoPlayerWindow& window)
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

OscillatingPlayback::~OscillatingPlayback()
{
	if (active())
		(void)stop();
}

bool OscillatingPlayback::start(const OscillationRequest& request, bool playWhenReady, QString* immediateError)
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

OscillationStopResult OscillatingPlayback::stop()
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

void OscillatingPlayback::setPlaying(bool playing)
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

void OscillatingPlayback::setMaximumSpeed(double speed)
{
	if (std::isfinite(speed) && speed > 0)
		_maximumSpeed = speed;
}

void OscillatingPlayback::setCurve(OscillationCurve curve)
{
	_curve = curve;
}

void OscillatingPlayback::resetElapsedBaselineAfterGuiBlock()
{
	if (_state == State::Playing)
		_elapsedTimer.restart();
}

void OscillatingPlayback::startPreparationProcess()
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

void OscillatingPlayback::consumeMjpegParts()
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

void OscillatingPlayback::finishPreparation(int exitCode, QProcess::ExitStatus exitStatus)
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

void OscillatingPlayback::fail(const QString& message)
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
	const QString diagnostics = QString::fromUtf8(_stderr).trimmed();
	_state = State::Inactive;
	clearCacheAndParser();
	_cancellingPreparation = false;
	_window.onOscillationFailed(message, diagnostics, displayedPosition, hasDisplayedPosition, shouldResumePlayback);
}

void OscillatingPlayback::advanceAndPresent()
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

void OscillatingPlayback::presentCurrentPhase()
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

bool OscillatingPlayback::presentFrame(int frameIndex, QString* error)
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

void OscillatingPlayback::clearCacheAndParser()
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
