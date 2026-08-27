#include "Windows/CompareWindow.h"
#include "Windows/ImageViewerWindow.h"
#include "UiComponents/ThumbnailWidget.h"
#include "Utils.h"
#include "assert/advanced_assert.h"
#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QShortcut>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>
RESTORE_COMPILER_WARNINGS

static constexpr int COMPARE_CELL_HEIGHT = 500;
static constexpr int DEBOUNCE_MS = 50;

CompareWindow::CompareWindow(const QStringList& folderPaths, QWidget* parent) : QWidget(parent, Qt::Window)
{
	setWindowTitle(tr("Compare Frames"));
	resize(1200, COMPARE_CELL_HEIGHT + 60);

	for (const QString& folder : folderPaths) {
		QDir dir(folder);
		QStringList files = listFrameImageFiles(dir);
		for (QString& f : files)
			f = dir.filePath(f);
		_folderFrames.push_back(std::move(files));
	}

	int maxFrameCount = 0;
	for (const QStringList& frames : std::as_const(_folderFrames))
		maxFrameCount = qMax(maxFrameCount, static_cast<int>(frames.size()));

	QHBoxLayout* thumbnailRow = new QHBoxLayout();
	for (int i = 0; i < folderPaths.size(); ++i)
	{
		_thumbnailWidgets.push_back(new ThumbnailWidget(QString{}, QString{}, 0, this));
		// A pane shows one folder's frame at the slider position, so the viewer browses that folder from there.
		// It only fires for a pane that has a frame loaded, so the slider position is in range for that folder.
		_thumbnailWidgets.back()->setOnActivatedCallback([this, i] {
			ImageViewerWindow::showForImages(nullptr, _folderFrames[static_cast<size_t>(i)], _slider->value(), this);
		});
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
			widget->loadFrame(QString{}, QString{});
	}
}
