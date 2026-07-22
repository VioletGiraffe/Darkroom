#include "Windows/CompareWindow.h"
#include "UiComponents/ThumbnailWidget.h"
#include "Utils.h"
#include "assert/advanced_assert.h"

#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QSlider>
#include <QShortcut>
#include <QTimer>
#include <QVBoxLayout>

static constexpr int COMPARE_CELL_HEIGHT = 500;
static constexpr int DEBOUNCE_MS = 50;

CompareWindow::CompareWindow(const QStringList& folderPaths, QWidget* parent) : QWidget(parent, Qt::Window)
{
	setWindowTitle(tr("Compare Frames"));
	resize(1200, COMPARE_CELL_HEIGHT + 60);

	// Collect sorted frame lists for each folder.
	for (const QString& folder : folderPaths) {
		QDir dir(folder);
		QStringList files = listFrameImageFiles(dir);
		// Prepend the full path so ThumbnailWidget can load them directly.
		for (QString& f : files)
			f = dir.filePath(f);
		_folderFrames.push_back(std::move(files));
	}

	// The slider spans 0 .. maxFrameCount-1 (the longest folder); a folder with no frame at the current
	// index just shows an empty cell (see loadCurrentFrame).
	int maxFrameCount = 0;
	for (const QStringList& frames : std::as_const(_folderFrames))
		maxFrameCount = qMax(maxFrameCount, static_cast<int>(frames.size()));

	QHBoxLayout* thumbnailRow = new QHBoxLayout();
	// No initial paths, no size required here - the first load will happen on resize
	for (int i = 0; i < folderPaths.size(); ++i)
	{
		_thumbnailWidgets.push_back(new ThumbnailWidget(QString{}, QString{}, 0, this));
		thumbnailRow->addWidget(_thumbnailWidgets.back());
	}

	_slider = new QSlider(Qt::Horizontal, this);
	_slider->setMinimum(0);
	_slider->setMaximum(qMax(0, maxFrameCount - 1));
	_slider->setValue(0);
	_slider->setPageStep(5);

	_frameLabel = new QLabel(tr("Frame: 1 / %1").arg(maxFrameCount), this);
	_frameLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	_frameLabel->setMinimumWidth(120);

	QHBoxLayout* sliderRow = new QHBoxLayout();
	sliderRow->addWidget(_slider);
	sliderRow->addWidget(_frameLabel);

	QVBoxLayout* mainLayout = new QVBoxLayout(this);
	mainLayout->addLayout(thumbnailRow, 1);
	mainLayout->addLayout(sliderRow, 0);

	// Debounce timer: single-shot, fires after DEBOUNCE_MS of slider inactivity.
	_debounceTimer = new QTimer(this);
	_debounceTimer->setSingleShot(true);
	connect(_debounceTimer, &QTimer::timeout, this, &CompareWindow::loadCurrentFrame);

	connect(_slider, &QSlider::valueChanged, this, [this, maxFrameCount](int value) {
		_frameLabel->setText(tr("Frame: %1 / %2").arg(value + 1).arg(maxFrameCount));
		startFrameLoadTimer();
	});

	QShortcut* escShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), this);
	connect(escShortcut, &QShortcut::activated, this, &CompareWindow::close);

	restoreWindowGeometry(this, "compareWindow");
}

CompareWindow::~CompareWindow() {
	saveWindowGeometry(this, "compareWindow");
}

void CompareWindow::resizeEvent(QResizeEvent* event)
{
	QWidget::resizeEvent(event);
	if (_debounceTimer)
		startFrameLoadTimer();
}

void CompareWindow::startFrameLoadTimer()
{
	_debounceTimer->start(DEBOUNCE_MS);
}

void CompareWindow::loadCurrentFrame()
{
	const int frameIndex = _slider->value();

	QStringList paths;
	paths.reserve(_folderFrames.size());

	assert_r(_thumbnailWidgets.size() == _folderFrames.size());

	for (size_t i = 0; i < _thumbnailWidgets.size(); ++i)
	{
		ThumbnailWidget* widget = _thumbnailWidgets[i];
		if (frameIndex < _folderFrames[i].size())
		{
			const QString framePath = _folderFrames[i][frameIndex];
			QString caption = QFileInfo(framePath).fileName();
			widget->loadFrame(framePath, caption);
		}
		else
			widget->loadFrame(QString{}, QString{}); // Clear the thumbnail if there's no matching frame index
	}
}
