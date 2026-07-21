#include "Windows/VideoPlayerWindow.h"
#include "UiComponents/MarkerSlider.h"
#include "Core/Library.h"
#include "Core/MetadataStore.h"
#include "Theme/Icons.h"
#include "Settings.h"

#include <QAudio>
#include <QAudioOutput>
#include <QCheckBox>
#include <QComboBox>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QMediaPlayer>
#include <QPushButton>
#include <QSettings>
#include <QScreen>
#include <QShortcut>
#include <QSlider>
#include <QTime>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QVideoSink>
#include <QVideoWidget>

#include <algorithm>

namespace {
// Item-data roles under which a saved-loop combo entry stores its interval endpoints (ms).
enum LoopItemDataRole { LoopStartRole = Qt::UserRole, LoopEndRole = Qt::UserRole + 1 };
}

std::vector<VideoPlayerWindow*> VideoPlayerWindow::_instances;


VideoPlayerWindow::VideoPlayerWindow(Library& library, const QString& videoPath, const MediaId& mediaId, QWidget* parent)
	: QMainWindow(parent), _library(library), _mediaId(mediaId)
{
	setAttribute(Qt::WA_DeleteOnClose);
	setWindowTitle(QFileInfo{ videoPath }.completeBaseName());

	_instances.push_back(this);
	// Initialize Player & Output
	_videoWidget = new QVideoWidget(this);
	_player = new QMediaPlayer(this);
	_audioOutput = new QAudioOutput(this);
	_player->setVideoOutput(_videoWidget);
	_player->setAudioOutput(_audioOutput); // Qt6 QMediaPlayer is silent until an audio sink is attached
	_player->setSource(QUrl::fromLocalFile(videoPath));

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
	MarkerSlider* slider = new MarkerSlider(Qt::Horizontal, this);
	QLabel* timeLabel = new QLabel(tr("video is loading..."), this);

	static constexpr double speeds[] { 0.25, 0.35, 0.5, 0.6, 0.8, 1.0, 2.0 };
	QComboBox* speedCombo = new QComboBox(this);
	for (const auto& s : speeds)
		speedCombo->addItem(QString::number(s) + "×", s);

	_pauseOnSeek = QSettings{}.value(Settings::PauseOnSeek, Defaults::PauseOnSeek).toBool();

	// Restore persisted speed, default to 1.0×
	{
		const double savedSpeed = QSettings{}.value(Settings::PlaybackSpeed, 1.0).toDouble();
		for (int i = 0; i < speedCombo->count(); ++i)
		{
			if (qAbs(speedCombo->itemData(i).toDouble() - savedSpeed) < 0.001)
			{
				speedCombo->setCurrentIndex(i);
				_player->setPlaybackRate(speedCombo->currentData().toDouble());
				break;
			}
		}
	}

	auto* pauseOnSeekCheck = new QCheckBox(tr("Pause on seek"), this);
	pauseOnSeekCheck->setChecked(_pauseOnSeek);

	connect(pauseOnSeekCheck, &QCheckBox::toggled, this, [this](bool checked) {
		_pauseOnSeek = checked;
		QSettings{}.setValue(Settings::PauseOnSeek, checked);
	});

	// Volume: the slider is a perceptual 0..100 position; the audio device wants a linear gain, so map through
	// QAudio::convertVolume. Mute is an independent flag on the sink and leaves the volume position untouched.
	auto* volumeSlider = new QSlider(Qt::Horizontal, this);
	volumeSlider->setRange(0, 100);
	volumeSlider->setFixedWidth(90);
	volumeSlider->setToolTip(tr("Volume"));

	auto* muteButton = new QPushButton(this);
	muteButton->setCheckable(true);
	muteButton->setToolTip(tr("Mute"));

	const auto applyVolume = [this](int position) {
		_audioOutput->setVolume(QAudio::convertVolume(position / qreal(100), QAudio::LogarithmicVolumeScale, QAudio::LinearVolumeScale));
	};
	const auto updateMuteIcon = [muteButton] {
		muteButton->setIcon(Theme::tintedIcon(muteButton->isChecked() ? QStringLiteral(":/UI/icon_volume_muted.svg")
		                                                               : QStringLiteral(":/UI/icon_volume.svg"),
		                                       &Theme::ThemeColors::TextPrimary));
	};

	// Restore persisted volume/mute directly; the handlers below are wired afterwards, so this doesn't re-persist.
	{
		const int savedVolume = QSettings{}.value(Settings::Volume, Defaults::Volume).toInt();
		const bool savedMuted = QSettings{}.value(Settings::Muted, Defaults::Muted).toBool();
		volumeSlider->setValue(savedVolume);
		applyVolume(savedVolume);
		muteButton->setChecked(savedMuted);
		_audioOutput->setMuted(savedMuted);
		updateMuteIcon();
	}

	connect(volumeSlider, &QAbstractSlider::valueChanged, this, [applyVolume](int position) {
		applyVolume(position);
		QSettings{}.setValue(Settings::Volume, position);
	});
	connect(muteButton, &QPushButton::toggled, this, [this, updateMuteIcon](bool muted) {
		_audioOutput->setMuted(muted);
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
	auto* deleteLoopButton = new QPushButton(tr("Delete"), this);
	saveLoopButton->setToolTip(tr("Save the current loop for this video"));
	deleteLoopButton->setToolTip(tr("Delete the selected saved loop"));

	// Shared helpers. Capture widget pointers by value - safe, they're parented to this and outlive the ctor.
	const auto activateLoop = [this, slider, setLoopStartButton, setLoopEndButton](qint64 start, qint64 end) {
		_loopStart = start;
		_loopEnd = end;
		slider->setMarkerA(static_cast<int>(start));
		slider->setMarkerB(static_cast<int>(end));
		setLoopStartButton->setChecked(true);
		setLoopEndButton->setChecked(true);
	};
	const auto clearLoop = [this, slider, setLoopStartButton, setLoopEndButton] {
		_loopStart = _loopEnd = -1;
		slider->clearMarkers();
		setLoopStartButton->setChecked(false);
		setLoopEndButton->setChecked(false);
	};
	const auto addIntervalItem = [loopCombo](qint64 start, qint64 end) {
		const auto fmt = [](qint64 ms) {
			return QTime::fromMSecsSinceStartOfDay(static_cast<int>(ms)).toString(ms >= 3600000 ? "h:mm:ss" : "m:ss");
		};
		loopCombo->addItem(fmt(start) + " - " + fmt(end));
		const int index = loopCombo->count() - 1;
		loopCombo->setItemData(index, start, LoopStartRole);
		loopCombo->setItemData(index, end, LoopEndRole);
		return index;
	};
	const auto persistIntervals = [this, loopCombo] {
		QJsonArray array;
		for (int i = 1; i < loopCombo->count(); ++i) // skip index 0 ("No loop")
		{
			QJsonObject object;
			object.insert("start", loopCombo->itemData(i, LoopStartRole).toLongLong());
			object.insert("end", loopCombo->itemData(i, LoopEndRole).toLongLong());
			object.insert("name", loopCombo->itemText(i));
			array.append(object);
		}
		_library.metadataStore().beginBatch().set(_mediaId, u"intervals", array);  // single write; the temporary Writer flushes right here
	};

	// Load this video's saved loops into the combo without firing the activation handler wired below.
	{
		const QSignalBlocker blocker{ loopCombo };
		const QJsonArray saved = _library.metadataStore().get(_mediaId, u"intervals").toArray();
		for (const QJsonValue& value : saved)
		{
			const QJsonObject object = value.toObject();
			addIntervalItem(object.value("start").toInteger(), object.value("end").toInteger());
		}
	}

	connect(setLoopStartButton, &QPushButton::clicked, this, [this, slider, setLoopStartButton] {
		_loopStart = _player->position();
		slider->setMarkerA(static_cast<int>(_loopStart));
		setLoopStartButton->setChecked(true);
	});
	connect(setLoopEndButton, &QPushButton::clicked, this, [this, slider, setLoopEndButton] {
		_loopEnd = _player->position();
		slider->setMarkerB(static_cast<int>(_loopEnd));
		setLoopEndButton->setChecked(true);
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
		             loopCombo->itemData(index, LoopEndRole).toLongLong());
	});
	connect(saveLoopButton, &QPushButton::clicked, this, [this, loopCombo, addIntervalItem, persistIntervals] {
		if (_loopStart < 0 || _loopEnd <= _loopStart)
			return; // nothing valid to save
		const int index = addIntervalItem(_loopStart, _loopEnd);
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

	// Keyboard equivalents for the loop endpoint buttons; route through click() to reuse their handlers.
	QShortcut* setLoopStartShortcut = new QShortcut(QKeySequence(Qt::Key_BracketLeft), this);
	connect(setLoopStartShortcut, &QShortcut::activated, setLoopStartButton, &QPushButton::click);
	QShortcut* setLoopEndShortcut = new QShortcut(QKeySequence(Qt::Key_BracketRight), this);
	connect(setLoopEndShortcut, &QShortcut::activated, setLoopEndButton, &QPushButton::click);

	controlsLayout->addWidget(slider);
	controlsLayout->addWidget(timeLabel);
	controlsLayout->addWidget(speedCombo);
	controlsLayout->addWidget(pauseOnSeekCheck);
	controlsLayout->addWidget(muteButton);
	controlsLayout->addWidget(volumeSlider);

	// Loop controls live on their own row to keep the seek row uncluttered.
	QHBoxLayout* loopLayout = new QHBoxLayout();
	loopLayout->setSpacing(6);
	loopLayout->addWidget(new QLabel(tr("Loop:"), this));
	loopLayout->addWidget(setLoopStartButton);
	loopLayout->addWidget(setLoopEndButton);
	loopLayout->addWidget(clearLoopButton);
	loopLayout->addWidget(loopCombo, 1);
	loopLayout->addWidget(saveLoopButton);
	loopLayout->addWidget(deleteLoopButton);

	connect(speedCombo, &QComboBox::currentIndexChanged, this, [this, speedCombo](int index) {
		if (index < 0)
			return;
		const double speed = speedCombo->itemData(index).toDouble();
		_player->setPlaybackRate(speed);
		QSettings{}.setValue(Settings::PlaybackSpeed, speed);
	});

	mainLayout->addLayout(controlsLayout);
	mainLayout->addLayout(loopLayout);

	setCentralWidget(centralWidget);

	// Configure Looping
	_player->setLoops(QMediaPlayer::Infinite);

	// Progress Tracking & Seeking Connections
	connect(_player, &QMediaPlayer::durationChanged, this, [slider](qint64 duration) {
		slider->setRange(0, static_cast<int>(duration));
	});

	connect(_player, &QMediaPlayer::positionChanged, this, [this, slider, timeLabel](qint64 position) {
		// A-B loop: jump back to the start once playback reaches the end marker.
		// The follow-up positionChanged from the seek refreshes the slider/label below.
		if (_loopStart >= 0 && _loopEnd > _loopStart && position >= _loopEnd)
		{
			_player->setPosition(_loopStart);
			return;
		}

		// Prevent the slider from snapping/jumping around while actively being dragged
		if (!slider->isSliderDown())
		{
			const QSignalBlocker blocker{ slider };
			slider->setValue(static_cast<int>(position));
		}

		const qint64 duration = _player->duration();
		const QTime posTime = QTime::fromMSecsSinceStartOfDay(static_cast<int>(position));
		const QTime durTime = QTime::fromMSecsSinceStartOfDay(static_cast<int>(duration));

		// Adapt format if video is longer than an hour
		const QString format = (duration >= 3600000) ? "hh:mm:ss.zzz" : "mm:ss.zzz";
		timeLabel->setText(QString("%1 / %2").arg(posTime.toString(format), durTime.toString(format)));
	});

	// Mouse drag: always pause on press, resume on release if pause-on-seek is off.
	// Keyboard seeks don't fire sliderPressed/Released, so valueChanged handles them.
	connect(slider, &QAbstractSlider::sliderPressed, this, [this] {
		_wasPlayingBeforeSeek = (_player->playbackState() == QMediaPlayer::PlayingState);
		_player->pause();
	});
	connect(slider, &QAbstractSlider::valueChanged, this, [this, slider](int value) {
		_player->setPosition(value);
		if (!slider->isSliderDown() && _pauseOnSeek)
			_player->pause();
	});
	connect(slider, &QAbstractSlider::sliderReleased, this, [this] {
		if (!_pauseOnSeek && _wasPlayingBeforeSeek)
			_player->play();
	});

	// Resize to the video size
	connect(_videoWidget->videoSink(), &QVideoSink::videoSizeChanged, this, &VideoPlayerWindow::resizeAndMoveWindow);

	// Spacebar Play/Pause Toggle
	QShortcut* spaceShortcut = new QShortcut(QKeySequence(Qt::Key_Space), this);
	connect(spaceShortcut, &QShortcut::activated, this, &VideoPlayerWindow::togglePlayPause);

	QShortcut* closeWindowShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), this);
	connect(closeWindowShortcut, &QShortcut::activated, this, &VideoPlayerWindow::close);

	// Start playback
	_player->play();
}

VideoPlayerWindow::~VideoPlayerWindow()
{
	std::erase(_instances, this);
}

void VideoPlayerWindow::restartAll()
{
	for (VideoPlayerWindow* win : _instances)
	{
		win->_player->pause();
		win->_player->setPosition(0);
	}

	// Small delay to ensure all players have reset before starting playback again
	QTimer::singleShot(100, [] {
		for (VideoPlayerWindow* win : _instances)
		{
			win->_player->play();
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
	QSize s = _videoWidget->videoSink()->videoSize();
	s.setWidth(qBound(300, s.width(), 1280));
	s.setHeight(qBound(200, s.height(), 720));
	// Temporarily force the layout to accommodate the exact video size
	_videoWidget->setMinimumSize(s);
	updateGeometry();
	adjustSize();

	QScreen* screen = QGuiApplication::primaryScreen();
	const QRect screenRect = screen ? screen->availableGeometry() : QRect(0, 0, 1600, 900);
	const int thirdWidth = screenRect.width() / 3;

	// Scan actual window positions to see which zones are occupied
	size_t occupiedZones[3]{ 0, 0, 0 };

	for (VideoPlayerWindow* win : _instances)
	{
		if (win == this)
			continue; // Skip myself

		// Find which third of the screen this window's center is currently in
		const int centerX = win->geometry().center().x() - screenRect.x();
		if (centerX < thirdWidth)
			occupiedZones[0] += 1;
		else if (centerX < thirdWidth * 2)
			occupiedZones[1] += 1;
		else
			occupiedZones[2] += 1;
	}

	// Find the least occupied zone
	int targetZone = 1; // Prefer center
	if (occupiedZones[0] + occupiedZones[1] + occupiedZones[2] != 0)
		targetZone = std::ranges::min_element(occupiedZones) - std::begin(occupiedZones);

	// Position the window in the center of the target zone.
	// Calculate X to center the window inside its designated third
	const int targetX = screenRect.left() + (targetZone * thirdWidth) + qMax(0, thirdWidth - s.width()) / 2;
	// Center vertically on the screen
	const int targetY = screenRect.top() + qMax(0, screenRect.height() - s.height()) / 2;
	move(targetX, targetY);

	// Immediately* drop the constraint
	QTimer::singleShot(150, this, [this]() { _videoWidget->setMinimumSize(0, 0); });
}

void VideoPlayerWindow::togglePlayPause()
{
	if (_player->playbackState() == QMediaPlayer::PlayingState)
		_player->pause();
	else
		_player->play();
}

bool VideoPlayerWindow::eventFilter(QObject* watched, QEvent* event)
{
	// Pause/play on mouse button click
	if (event->type() == QEvent::MouseButtonRelease)
	{
		togglePlayPause();
		return true; // Consume the event so it doesn't propagate to the widget
	}

	return QMainWindow::eventFilter(watched, event);
}
