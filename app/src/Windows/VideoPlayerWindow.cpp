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
#include <QMediaPlayer>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QSettings>
#include <QScreen>
#include <QShortcut>
#include <QSlider>
#include <QTemporaryDir>
#include <QTime>
#include <QTimer>
#include <QToolTip>
#include <QUrl>
#include <QVBoxLayout>
#include <QVideoSink>
#include <QVideoWidget>
#include <QWheelEvent>

#include <algorithm>
#include <limits>
#include <optional>
#include <stdint.h>

namespace {
// Item-data roles under which a saved-loop combo entry stores its interval endpoints (ms) and optional name.
enum LoopItemDataRole { LoopStartRole = Qt::UserRole, LoopEndRole = Qt::UserRole + 1, LoopNameRole = Qt::UserRole + 2 };

// A saved loop's dropdown label: optional name first, then the start as a clock time and the duration as
// fractional seconds to 1 digit (e.g. "warmup   0:12 + 2.3s").
QString formatLoopLabel(qint64 startMs, qint64 endMs, const QString& name)
{
	const QString start = QTime::fromMSecsSinceStartOfDay(static_cast<int>(startMs)).toString(startMs >= 3600000 ? "h:mm:ss" : "m:ss");
	const QString times = start + " + " + QString::number((endMs - startMs) / 1000.0, 'f', 1) + "s";
	return name.isEmpty() ? times : name + "   " + times;
}

// Settings::LastFrameExtractionMode values.
constexpr const char* ExtractToLibraryMode = "library";
constexpr const char* ExtractToFolderMode  = "folder";

// Volume-slider units (0..100) changed per wheel notch over the video.
constexpr int VolumeWheelStep = 5;
}

std::vector<VideoPlayerWindow*> VideoPlayerWindow::_instances;


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

	_pauseOnSeek = settings.value(Settings::PauseOnSeek, Defaults::PauseOnSeek).toBool();

	// Restore persisted speed, default to 1.0×
	{
		const double savedSpeed = settings.value(Settings::PlaybackSpeed, 1.0).toDouble();
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
	_volumeSlider = new QSlider(Qt::Horizontal, this);
	_volumeSlider->setRange(0, 100);
	_volumeSlider->setFixedWidth(90);
	_volumeSlider->setToolTip(tr("Volume"));

	auto* muteButton = new QPushButton(this);
	muteButton->setCheckable(true);
	muteButton->setToolTip(tr("Mute (M)"));

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
		const bool savedMuted = settings.value(Settings::Muted, Defaults::Muted).toBool();
		_volumeSlider->setValue(savedVolume);
		applyVolume(savedVolume);
		muteButton->setChecked(savedMuted);
		_audioOutput->setMuted(savedMuted);
		updateMuteIcon();
	}

	connect(_volumeSlider, &QAbstractSlider::valueChanged, this, [applyVolume](int position) {
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
	auto* renameLoopButton = new QPushButton(tr("Rename"), this);
	auto* deleteLoopButton = new QPushButton(tr("Delete"), this);
	saveLoopButton->setToolTip(tr("Save the current loop for this video"));
	renameLoopButton->setToolTip(tr("Rename the selected saved loop"));
	deleteLoopButton->setToolTip(tr("Delete the selected saved loop"));

	// Shared helpers. Capture widget pointers by value - safe, they're parented to this and outlive the ctor.
	const auto activateLoop = [this, slider, setLoopStartButton, setLoopEndButton](qint64 start, qint64 end) {
		_loopStart = start;
		_loopEnd = end;
		slider->setMarkerA(static_cast<int>(start));
		slider->setMarkerB(static_cast<int>(end));
		setLoopStartButton->setChecked(true);
		setLoopEndButton->setChecked(true);
		_player->setPosition(start);  // rewind so looping starts now, rather than after the playhead drifts past the end
	};
	const auto clearLoop = [this, slider, setLoopStartButton, setLoopEndButton] {
		_loopStart = _loopEnd = -1;
		slider->clearMarkers();
		setLoopStartButton->setChecked(false);
		setLoopEndButton->setChecked(false);
	};
	const auto addIntervalItem = [loopCombo](qint64 start, qint64 end, const QString& name) {
		loopCombo->addItem(formatLoopLabel(start, end, name));
		const int index = loopCombo->count() - 1;
		loopCombo->setItemData(index, start, LoopStartRole);
		loopCombo->setItemData(index, end, LoopEndRole);
		loopCombo->setItemData(index, name, LoopNameRole);
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
			addIntervalItem(object.value("start").toInteger(), object.value("end").toInteger(), object.value("name").toString());
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
	connect(saveLoopButton, &QPushButton::clicked, this, [this, loopCombo, addIntervalItem, persistIntervals, promptLoopName] {
		if (_loopStart < 0 || _loopEnd <= _loopStart)
			return; // nothing valid to save
		const std::optional<QString> name = promptLoopName(tr("Save loop"), {});
		if (!name)
			return; // cancelled
		const int index = addIntervalItem(_loopStart, _loopEnd, *name);
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
		loopCombo->setItemText(index, formatLoopLabel(start, end, *name));
		loopCombo->setItemData(index, *name, LoopNameRole);
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
	controlsLayout->addWidget(_volumeSlider);

	// Loop controls live on their own row to keep the seek row uncluttered.
	QHBoxLayout* loopLayout = new QHBoxLayout();
	loopLayout->setSpacing(6);
	loopLayout->addWidget(new QLabel(tr("Loop:"), this));
	loopLayout->addWidget(setLoopStartButton);
	loopLayout->addWidget(setLoopEndButton);
	loopLayout->addWidget(clearLoopButton);
	loopLayout->addWidget(loopCombo, 1);
	loopLayout->addWidget(saveLoopButton);
	loopLayout->addWidget(renameLoopButton);
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
	connect(extractFrameShortcut, &QShortcut::activated, this, [this] { repeatLastExtraction(_player->position()); });

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

void VideoPlayerWindow::togglePlayPause()
{
	if (_player->playbackState() == QMediaPlayer::PlayingState)
		_player->pause();
	else
		_player->play();
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
	const qint64 timestampMs = _player->position();

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
