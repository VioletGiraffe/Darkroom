#include "Windows/VideoPlayerWindow.h"
#include "Windows/SingleFrameExtraction.h"
#include "Windows/OscillatingPlayback.h"
#include "UiComponents/MarkerSlider.h"
#include "Core/Catalog.h"
#include "Core/Library.h"
#include "Core/MetadataStore.h"
#include "Theme/Icons.h"
#include "Utils.h"
#include "assert/advanced_assert.h"
#include "dialogs/messagebox.h"

#include <QAudio>
#include <QAudioOutput>
#include <QCheckBox>
#include <QComboBox>
#include <QCursor>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHBoxLayout>
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
#include <QPushButton>
#include <QSettings>
#include <QScreen>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSlider>
#include <QTime>
#include <QTimer>
#include <QToolTip>
#include <QUrl>
#include <QVBoxLayout>
#include <QVideoSink>
#include <QVideoWidget>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <stdint.h>
#include <utility>

namespace Settings {
	constexpr const char* PauseOnSeek  = "VideoPlayer/PauseOnSeek";
	constexpr const char* Volume       = "VideoPlayer/Volume";
	constexpr const char* Muted        = "VideoPlayer/Muted";
}

namespace Defaults {
	constexpr bool PauseOnSeek = true;
	constexpr int  Volume      = 100;
	constexpr bool Muted       = false;
}

namespace {
// A zero speed preserves legacy loops saved before speed became an attribute.
enum LoopItemDataRole { LoopStartRole = Qt::UserRole, LoopEndRole = Qt::UserRole + 1, LoopNameRole = Qt::UserRole + 2, LoopSpeedRole = Qt::UserRole + 3 };

QString formatLoopLabel(qint64 startMs, qint64 endMs, const QString& name, double speed)
{
	const QString start = QTime::fromMSecsSinceStartOfDay(static_cast<int>(startMs)).toString(startMs >= 3600000 ? "h:mm:ss" : "m:ss");
	QString label = start + " + " + QString::number((endMs - startMs) / 1000.0, 'f', 1) + "s";
	if (speed > 0 && qAbs(speed - 1.0) > 0.001)
		label += " @" + QString::number(speed) + "×";
	return name.isEmpty() ? label : name + "   " + label;
}

constexpr int VolumeWheelStep = 5;
constexpr qint64 MaxSeekStepMs = 15000;

// QVideoWidget has no minimum size hint of its own, so without this the video area can be squeezed to nothing.
constexpr QSize MinVideoAreaSize{ 640, 400 };

// Together these keep worst-case cache memory acceptable; no separate byte cap is needed.
constexpr qint64 MaxOscillationDurationMs = 30000;
constexpr qreal MaxOscillationFrameRate = 60.0;
constexpr QSize MaxOscillationFrameSize{ 1920, 1080 };
constexpr int ExpectedFrameCountAllowance = 2;
constexpr const char* OscillationCurveSettingKey = "VideoPlayer/OscillationCurve";
constexpr const char* DefaultOscillationCurveSetting = "cosine";

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

}

std::vector<VideoPlayerWindow*> VideoPlayerWindow::_instances;


VideoPlayerWindow::VideoPlayerWindow(Library& library, const QString& videoPath, const MediaId& mediaId, QWidget* parent)
	: QMainWindow(parent), _library(library)
{
	setAttribute(Qt::WA_DeleteOnClose);

	const QSettings settings;

	_instances.push_back(this);
	_videoWidget = new QVideoWidget(this);
	_player = new QMediaPlayer(this);
	_audioOutput = new QAudioOutput(this);
	_player->setVideoOutput(_videoWidget);
	_player->setAudioOutput(_audioOutput);
	_oscillatingPlayback = std::make_unique<OscillatingPlayback>(*this);

	_videoWidget->installEventFilter(this);
	_videoWidget->setMinimumSize(MinVideoAreaSize);

	QWidget* centralWidget = new QWidget(this);
	QVBoxLayout* mainLayout = new QVBoxLayout(centralWidget);
	mainLayout->setContentsMargins(4, 0, 4, 4);
	mainLayout->setSpacing(0);
	mainLayout->addWidget(_videoWidget, 1);

	QHBoxLayout* controlsLayout = new QHBoxLayout();
	controlsLayout->setSpacing(6);
	_seekSlider = new MarkerSlider(Qt::Horizontal, this);
	_timeLabel = new QLabel(tr("video is loading..."), this);

	static constexpr double speeds[] { 0.25, 0.35, 0.5, 0.6, 0.8, 1.0, 1.5, 2.0, 2.5, 3.0, 4.0, 6.0, 8.0, 10.0 };
	_speedCombo = new QComboBox(this);
	_speedCombo->setToolTip(tr("Playback speed. During oscillation this is the approximate maximum speed."));
	for (const auto& s : speeds)
		_speedCombo->addItem(QString::number(s) + "×", s);

	_pauseOnSeek = settings.value(Settings::PauseOnSeek, Defaults::PauseOnSeek).toBool();

	connect(_speedCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
		if (index < 0)
			return;
		const double speed = _speedCombo->itemData(index).toDouble();
		applyPlaybackSpeed(speed);
		auto writer = _library.metadataStore().beginBatch();
		if (qAbs(speed - 1.0) < 0.001)
			writer.removeField(_mediaId, u"playbackSpeed");
		else
			writer.set(_mediaId, u"playbackSpeed", speed);
	});

	auto* pauseOnSeekCheck = new QCheckBox(tr("Pause on seek"), this);
	pauseOnSeekCheck->setChecked(_pauseOnSeek);

	connect(pauseOnSeekCheck, &QCheckBox::toggled, this, [this](bool checked) {
		_pauseOnSeek = checked;
		QSettings{}.setValue(Settings::PauseOnSeek, checked);
	});

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

	_loopStartButton = new QPushButton("A", this);
	_loopEndButton = new QPushButton("B", this);
	auto* clearLoopButton = new QPushButton(tr("Clear"), this);
	_loopStartButton->setCheckable(true);
	_loopEndButton->setCheckable(true);
	_loopStartButton->setToolTip(tr("Set loop start at the current position"));
	_loopEndButton->setToolTip(tr("Set loop end at the current position"));

	_savedLoopCombo = new QComboBox(this);
	_savedLoopCombo->setToolTip(tr("Saved loops for this video"));
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

	const auto activateLoop = [this](qint64 start, qint64 end, double speed) {
		exitOscillatingPlayback();
		_loopStart = start;
		_loopEnd = end;
		_seekSlider->setMarkerA(static_cast<int>(start));
		_seekSlider->setMarkerB(static_cast<int>(end));
		_loopStartButton->setChecked(true);
		_loopEndButton->setChecked(true);
		selectPlaybackSpeed(speed);
		_player->setPosition(start);
		updateOscillationAvailability();
	};
	const auto persistIntervals = [this] {
		QJsonArray array;
		for (int i = 1; i < _savedLoopCombo->count(); ++i)
		{
			QJsonObject object;
			object.insert("start", _savedLoopCombo->itemData(i, LoopStartRole).toLongLong());
			object.insert("end", _savedLoopCombo->itemData(i, LoopEndRole).toLongLong());
			object.insert("name", _savedLoopCombo->itemData(i, LoopNameRole).toString());
			object.insert("speed", _savedLoopCombo->itemData(i, LoopSpeedRole).toDouble());
			array.append(object);
		}
		_library.metadataStore().beginBatch().set(_mediaId, u"intervals", array);
	};
	const auto promptLoopName = [this](const QString& title, const QString& initial) -> std::optional<QString> {
		bool ok = false;
		const QString name = QInputDialog::getText(this, title, tr("Loop name (optional):"), QLineEdit::Normal, initial, &ok).trimmed();
		if (!ok)
			return std::nullopt;
		return name;
	};

	connect(_loopStartButton, &QPushButton::clicked, this, [this] {
		const qint64 position = currentPlaybackPosition();
		exitOscillatingPlayback();
		_loopStart = position;
		_seekSlider->setMarkerA(static_cast<int>(_loopStart));
		_loopStartButton->setChecked(true);
		updateOscillationAvailability();
	});
	connect(_loopEndButton, &QPushButton::clicked, this, [this] {
		const qint64 position = currentPlaybackPosition();
		exitOscillatingPlayback();
		_loopEnd = position;
		_seekSlider->setMarkerB(static_cast<int>(_loopEnd));
		_loopEndButton->setChecked(true);
		updateOscillationAvailability();
	});
	connect(clearLoopButton, &QPushButton::clicked, this, [this] {
		clearAbInterval();
		const QSignalBlocker blocker{ _savedLoopCombo };
		_savedLoopCombo->setCurrentIndex(0);
	});

	connect(_savedLoopCombo, &QComboBox::currentIndexChanged, this, [this, activateLoop](int index) {
		if (index <= 0)
		{
			clearAbInterval();
			return;
		}
		activateLoop(_savedLoopCombo->itemData(index, LoopStartRole).toLongLong(),
		             _savedLoopCombo->itemData(index, LoopEndRole).toLongLong(),
		             _savedLoopCombo->itemData(index, LoopSpeedRole).toDouble());
	});
	connect(saveLoopButton, &QPushButton::clicked, this, [this, persistIntervals, promptLoopName] {
		if (!hasAbInterval())
			return;
		const std::optional<QString> name = promptLoopName(tr("Save loop"), {});
		if (!name)
			return;
		const int index = addSavedLoopItem(_loopStart, _loopEnd, *name, _speedCombo->currentData().toDouble());
		persistIntervals();
		_savedLoopCombo->setCurrentIndex(index);
	});
	connect(deleteLoopButton, &QPushButton::clicked, this, [this, persistIntervals] {
		const int index = _savedLoopCombo->currentIndex();
		if (index <= 0)
			return;
		clearAbInterval();
		// Removing the current item would otherwise activate its neighbour.
		const QSignalBlocker blocker{ _savedLoopCombo };
		_savedLoopCombo->removeItem(index);
		_savedLoopCombo->setCurrentIndex(0);
		persistIntervals();
	});

	connect(renameLoopButton, &QPushButton::clicked, this, [this, promptLoopName, persistIntervals] {
		const int index = _savedLoopCombo->currentIndex();
		if (index <= 0)
			return;
		const std::optional<QString> name = promptLoopName(tr("Rename loop"), _savedLoopCombo->itemData(index, LoopNameRole).toString());
		if (!name)
			return;
		const qint64 start = _savedLoopCombo->itemData(index, LoopStartRole).toLongLong();
		const qint64 end   = _savedLoopCombo->itemData(index, LoopEndRole).toLongLong();
		const double speed = _savedLoopCombo->itemData(index, LoopSpeedRole).toDouble();
		_savedLoopCombo->setItemText(index, formatLoopLabel(start, end, *name, speed));
		_savedLoopCombo->setItemData(index, *name, LoopNameRole);
		persistIntervals();
	});

	QShortcut* setLoopStartShortcut = new QShortcut(QKeySequence(Qt::Key_BracketLeft), this);
	connect(setLoopStartShortcut, &QShortcut::activated, _loopStartButton, &QPushButton::click);
	QShortcut* setLoopEndShortcut = new QShortcut(QKeySequence(Qt::Key_BracketRight), this);
	connect(setLoopEndShortcut, &QShortcut::activated, _loopEndButton, &QPushButton::click);

	controlsLayout->addWidget(_seekSlider);
	controlsLayout->addWidget(_timeLabel);
	controlsLayout->addWidget(_speedCombo);
	controlsLayout->addWidget(pauseOnSeekCheck);
	controlsLayout->addWidget(muteButton);
	controlsLayout->addWidget(_volumeSlider);

	QHBoxLayout* loopLayout = new QHBoxLayout();
	loopLayout->setSpacing(6);
	loopLayout->addWidget(new QLabel(tr("Loop:"), this));
	loopLayout->addWidget(_loopStartButton);
	loopLayout->addWidget(_loopEndButton);
	loopLayout->addWidget(clearLoopButton);
	loopLayout->addWidget(_oscillationCheck);
	loopLayout->addWidget(_oscillationCurveCombo);
	loopLayout->addWidget(_savedLoopCombo, 1);
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

	_player->setLoops(QMediaPlayer::Infinite);
	connect(_player, &QMediaPlayer::errorOccurred, this, [this](QMediaPlayer::Error error, const QString& details) {
		if (error == QMediaPlayer::NoError || _fatalPlaybackErrorReported)
			return;
		if (error == QMediaPlayer::FormatError)
		{
			if (_formatWarningReported)
				return;
			_pendingFormatError = details;
			resolvePendingFormatError();
			return;
		}
		reportFatalPlaybackError(details);
	});
	connect(_player, &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus status) {
		if (status == QMediaPlayer::InvalidMedia)
			reportFatalPlaybackError(_pendingFormatError.value_or(_player->errorString()));
		else
			resolvePendingFormatError();
	});
	connect(_player, &QMediaPlayer::hasVideoChanged, this, [this] { resolvePendingFormatError(); });

	connect(_player, &QMediaPlayer::durationChanged, this, [this](qint64 duration) {
		_seekSlider->setRange(0, static_cast<int>(duration));
		_seekSlider->setSingleStep(static_cast<int>(qMin(duration / 100, MaxSeekStepMs)));
		updateOscillationAvailability();
	});

	connect(_player, &QMediaPlayer::positionChanged, this, [this](qint64 position) {
		if (!_oscillatingPlayback->active() && hasAbInterval() && position >= _loopEnd)
		{
			_player->setPosition(_loopStart);
			return;
		}
		if (!_oscillatingPlayback->active())
			updatePlaybackPositionUi(position);
	});

	// Keyboard seeks do not emit sliderPressed/sliderReleased.
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

	connect(_videoWidget->videoSink(), &QVideoSink::videoSizeChanged, this, [this] {
		resizeAndMoveWindow();
		updateOscillationAvailability();
	});

	QShortcut* spaceShortcut = new QShortcut(QKeySequence(Qt::Key_Space), this);
	connect(spaceShortcut, &QShortcut::activated, this, &VideoPlayerWindow::togglePlayPause);

	// Stepping the slider reuses its seek handling: oscillation exit and pause-on-seek.
	QShortcut* seekBackShortcut = new QShortcut(QKeySequence(Qt::Key_Left), this);
	connect(seekBackShortcut, &QShortcut::activated, this, [this] { _seekSlider->triggerAction(QAbstractSlider::SliderSingleStepSub); });
	QShortcut* seekForwardShortcut = new QShortcut(QKeySequence(Qt::Key_Right), this);
	connect(seekForwardShortcut, &QShortcut::activated, this, [this] { _seekSlider->triggerAction(QAbstractSlider::SliderSingleStepAdd); });

	QShortcut* previousFileShortcut = new QShortcut(QKeySequence(Qt::Key_PageUp), this);
	connect(previousFileShortcut, &QShortcut::activated, this, [this] { loadAdjacentFile(-1); });
	QShortcut* nextFileShortcut = new QShortcut(QKeySequence(Qt::Key_PageDown), this);
	connect(nextFileShortcut, &QShortcut::activated, this, [this] { loadAdjacentFile(1); });

	QShortcut* closeWindowShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), this);
	connect(closeWindowShortcut, &QShortcut::activated, this, [this] {
		if (isFullScreen())
			showNormal();
		else
			close();
	});

	QShortcut* fullScreenShortcut = new QShortcut(QKeySequence(Qt::Key_F), this);
	connect(fullScreenShortcut, &QShortcut::activated, this, &VideoPlayerWindow::toggleFullScreen);

	QShortcut* muteShortcut = new QShortcut(QKeySequence(Qt::Key_M), this);
	connect(muteShortcut, &QShortcut::activated, muteButton, &QAbstractButton::toggle);

	QShortcut* extractFrameShortcut = new QShortcut(QKeySequence(Qt::Key_E), this);
	connect(extractFrameShortcut, &QShortcut::activated, this, [this] { repeatLastExtraction(currentPlaybackPosition()); });

	loadFile(videoPath, mediaId);
}

void VideoPlayerWindow::loadFile(const QString& videoPath, const MediaId& mediaId)
{
	exitOscillatingPlayback(false);
	_player->stop();

	// The metadata below is keyed by the new identity, so it has to be in place first.
	_videoPath = videoPath;
	_mediaId = mediaId;
	setWindowTitle(QFileInfo{ videoPath }.completeBaseName());

	_pendingFormatError.reset();
	_formatWarningReported = false;
	_fatalPlaybackErrorReported = false;

	clearAbInterval();
	{
		const QSignalBlocker blocker{ _savedLoopCombo };
		_savedLoopCombo->clear();
		_savedLoopCombo->addItem(tr("No loop"));
		const QJsonArray saved = _library.metadataStore().get(_mediaId, u"intervals").toArray();
		for (const QJsonValue& value : saved)
		{
			const QJsonObject object = value.toObject();
			addSavedLoopItem(object.value("start").toInteger(), object.value("end").toInteger(),
			                 object.value("name").toString(), object.value("speed").toDouble());
		}
	}

	const double storedSpeed = _library.metadataStore().get(_mediaId, u"playbackSpeed").toDouble();
	selectPlaybackSpeed(storedSpeed > 0 ? storedSpeed : 1.0);

	{
		// The new duration only arrives with durationChanged; until then the slider would span the previous video.
		const QSignalBlocker blocker{ _seekSlider };
		_seekSlider->setRange(0, 0);
	}
	_timeLabel->setText(tr("video is loading..."));

	_player->setSource(QUrl::fromLocalFile(videoPath));
	if (!_fatalPlaybackErrorReported)
		setPlaybackActive(true);
}

void VideoPlayerWindow::setNavigationOrder(std::vector<MediaId> order)
{
	_navigationOrder = std::move(order);
}

std::optional<MediaId> VideoPlayerWindow::adjacentMediaItem(int step) const
{
	assert_and_return_r(step == 1 || step == -1, std::nullopt);

	const auto current = std::find(_navigationOrder.cbegin(), _navigationOrder.cend(), _mediaId);
	if (current == _navigationOrder.cend())
		return std::nullopt;

	const Catalog& catalog = _library.catalog();
	for (int index = static_cast<int>(current - _navigationOrder.cbegin()) + step;
	     index >= 0 && index < static_cast<int>(_navigationOrder.size()); index += step)
	{
		// The order is a snapshot from when the player opened; a since-deleted item must not block navigation past it.
		const MediaId& candidate = _navigationOrder[static_cast<size_t>(index)];
		if (catalog.containsMediaItem(candidate) && QFileInfo::exists(catalog.sourcePathForMediaItem(candidate)))
			return candidate;
	}
	return std::nullopt;
}

void VideoPlayerWindow::loadAdjacentFile(int step)
{
	const std::optional<MediaId> mediaId = adjacentMediaItem(step);
	if (!mediaId)
		return;

	loadFile(_library.catalog().sourcePathForMediaItem(*mediaId), *mediaId);
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

	// Let every paused seek settle before restarting.
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
	targetVideoSize = targetVideoSize.expandedTo(MinVideoAreaSize);

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
	// The target size was only a lever for adjustSize.
	_videoWidget->setMinimumSize(MinVideoAreaSize);

	if (frameSize().width() > available.width() || frameSize().height() > available.height())
	{
		const QSize decorationSize = frameSize() - size();
		resize(qMax(1, available.width() - decorationSize.width()), qMax(1, available.height() - decorationSize.height()));
	}

	constexpr int preferredZoneOrder[]{ 1, 0, 2 };
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

void VideoPlayerWindow::applyPlaybackSpeed(double speed)
{
	_player->setPlaybackRate(speed);
	_oscillatingPlayback->setMaximumSpeed(speed);
}

void VideoPlayerWindow::selectPlaybackSpeed(double speed)
{
	if (!(speed > 0))
		return;

	int nearest = -1;
	double nearestDiff = std::numeric_limits<double>::max();
	for (int i = 0; i < _speedCombo->count(); ++i)
	{
		const double diff = qAbs(_speedCombo->itemData(i).toDouble() - speed);
		if (diff < nearestDiff)
		{
			nearestDiff = diff;
			nearest = i;
		}
	}
	if (nearest < 0)
		return;

	const QSignalBlocker blocker{ _speedCombo };
	_speedCombo->setCurrentIndex(nearest);
	applyPlaybackSpeed(_speedCombo->itemData(nearest).toDouble());
}

void VideoPlayerWindow::clearAbInterval()
{
	exitOscillatingPlayback();
	_loopStart = _loopEnd = -1;
	_seekSlider->clearMarkers();
	_loopStartButton->setChecked(false);
	_loopEndButton->setChecked(false);
	updateOscillationAvailability();
}

int VideoPlayerWindow::addSavedLoopItem(qint64 startMs, qint64 endMs, const QString& name, double speed)
{
	_savedLoopCombo->addItem(formatLoopLabel(startMs, endMs, name, speed));
	const int index = _savedLoopCombo->count() - 1;
	_savedLoopCombo->setItemData(index, startMs, LoopStartRole);
	_savedLoopCombo->setItemData(index, endMs, LoopEndRole);
	_savedLoopCombo->setItemData(index, name, LoopNameRole);
	_savedLoopCombo->setItemData(index, speed, LoopSpeedRole);
	return index;
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
	const QString& error, const QString& diagnostics, qint64 displayedPosition, bool hasDisplayedPosition, bool shouldResumePlayback)
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
	MessageBox::notice(this, tr("Oscillating playback"), error, diagnostics);
}

void VideoPlayerWindow::resolvePendingFormatError()
{
	if (!_pendingFormatError || _formatWarningReported || _fatalPlaybackErrorReported)
		return;

	if (_player->mediaStatus() == QMediaPlayer::InvalidMedia)
	{
		const QString details = std::exchange(_pendingFormatError, std::nullopt).value();
		reportFatalPlaybackError(details);
		return;
	}
	if (_player->hasVideo())
	{
		const QString details = std::exchange(_pendingFormatError, std::nullopt).value();
		reportRecoverableFormatError(details);
		return;
	}

	const QMediaPlayer::MediaStatus status = _player->mediaStatus();
	if (status == QMediaPlayer::LoadedMedia || status == QMediaPlayer::BufferedMedia || status == QMediaPlayer::EndOfMedia)
	{
		const QString details = std::exchange(_pendingFormatError, std::nullopt).value();
		reportFatalPlaybackError(details);
	}
}

void VideoPlayerWindow::reportFatalPlaybackError(const QString& details)
{
	if (_fatalPlaybackErrorReported)
		return;

	_fatalPlaybackErrorReported = true;
	_pendingFormatError.reset();
	exitOscillatingPlayback(false);
	_player->stop();
	_timeLabel->setText(tr("Playback failed"));
	// The box appears after this returns, by which time the window may hold a different file.
	const QString fileName = QFileInfo{ _videoPath }.fileName();
	QTimer::singleShot(0, this, [this, details, fileName] {
		MessageBox::notice(this, tr("Video playback"),
			tr("Could not play \"%1\".").arg(fileName), details, QMessageBox::Critical);
	});
}

void VideoPlayerWindow::reportRecoverableFormatError(const QString& details)
{
	if (_formatWarningReported || _fatalPlaybackErrorReported)
		return;

	_formatWarningReported = true;
	QTimer::singleShot(0, this, [this, details] {
		if (!_fatalPlaybackErrorReported)
			MessageBox::notice(this, tr("Limited video playback"),
				tr("Some media content is unsupported, but the video can still be played."), details);
	});
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
	if (event->type() == QEvent::MouseButtonRelease)
	{
		const auto* mouseEvent = static_cast<const QMouseEvent*>(event);
		if (mouseEvent->button() == Qt::LeftButton)
			togglePlayPause();
		else if (mouseEvent->button() == Qt::RightButton)
			showContextMenu(mouseEvent->globalPosition().toPoint());
		return true;
	}

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
	// Playback may continue while the menu is open; preserve the clicked frame.
	const qint64 timestampMs = currentPlaybackPosition();

	QMenu menu(this);
	menu.addAction(tr("Extract frame and import to library"), this, [this, timestampMs] { extractFrameToLibrary(timestampMs); });
	menu.addAction(tr("Extract frame to folder..."), this, [this, timestampMs] {
		const QString folder = QFileDialog::getExistingDirectory(this, tr("Extract frame to folder"),
			SingleFrameExtraction::lastFolder());
		if (!folder.isEmpty())
			extractFrameToFolder(timestampMs, folder);
	});

	const SingleFrameExtraction::LastDestination lastDestination = SingleFrameExtraction::lastDestination();
	const QString lastFolder = SingleFrameExtraction::lastFolder();
	QString repeatText;
	if (lastDestination == SingleFrameExtraction::LastDestination::Library)
		repeatText = tr("Extract frame → library");
	else if (lastDestination == SingleFrameExtraction::LastDestination::Folder && !lastFolder.isEmpty())
		repeatText = tr("Extract frame → %1").arg(QDir::toNativeSeparators(lastFolder));

	QAction* repeatAction = menu.addAction((repeatText.isEmpty() ? tr("Extract frame (last used)") : repeatText) + "\tE",
		this, [this, timestampMs] { repeatLastExtraction(timestampMs); });
	repeatAction->setEnabled(!repeatText.isEmpty());

	menu.addSeparator();
	if (!_navigationOrder.empty())
	{
		QAction* previousFileAction = menu.addAction(tr("Previous video") + "\tPgUp", this, [this] { loadAdjacentFile(-1); });
		previousFileAction->setEnabled(adjacentMediaItem(-1).has_value());
		QAction* nextFileAction = menu.addAction(tr("Next video") + "\tPgDown", this, [this] { loadAdjacentFile(1); });
		nextFileAction->setEnabled(adjacentMediaItem(1).has_value());
		menu.addSeparator();
	}

	menu.addAction(revealInFileManagerActionText(), this, [this] {
		if (!revealInFileManager(_videoPath))
			reportMissingFile(this, _videoPath);
	});
	menu.addAction((isFullScreen() ? tr("Exit fullscreen") : tr("Fullscreen")) + "\tF", this, &VideoPlayerWindow::toggleFullScreen);

	menu.exec(globalPos);
}

void VideoPlayerWindow::repeatLastExtraction(qint64 timestampMs)
{
	const SingleFrameExtraction::LastDestination lastDestination = SingleFrameExtraction::lastDestination();
	if (lastDestination == SingleFrameExtraction::LastDestination::Library)
		extractFrameToLibrary(timestampMs);
	else if (lastDestination == SingleFrameExtraction::LastDestination::Folder)
	{
		const QString lastFolder = SingleFrameExtraction::lastFolder();
		if (!lastFolder.isEmpty())
			extractFrameToFolder(timestampMs, lastFolder);
	}
}

void VideoPlayerWindow::extractFrameToFolder(qint64 timestampMs, const QString& folder)
{
	SingleFrameExtraction::extractToFolderInteractive(_videoPath, timestampMs, folder,
		[this] { _oscillatingPlayback->resetElapsedBaselineAfterGuiBlock(); }, this);
}

void VideoPlayerWindow::extractFrameToLibrary(qint64 timestampMs)
{
	SingleFrameExtraction::extractToLibraryInteractive(_library, _videoPath, timestampMs,
		[this] { _oscillatingPlayback->resetElapsedBaselineAfterGuiBlock(); }, this);
}
