#include "Windows/CompareWindow.h"
#include "UiComponents/ThumbnailWidget.h"
#include "Utils.h"

#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QSlider>
#include <QShortcut>
#include <QTimer>
#include <QVBoxLayout>

#include <assert.h>

static constexpr int COMPARE_CELL_HEIGHT = 500;
static constexpr int DEBOUNCE_MS = 50;

CompareWindow::CompareWindow(const QStringList& folderPaths, QWidget* parent) : QWidget(parent, Qt::Window)
{
	setWindowTitle(tr("Compare Frames"));
	resize(1200, COMPARE_CELL_HEIGHT + 60);

	// Collect sorted frame lists for each folder.
	const QStringList& imageFilters = IMAGE_FILE_FILTERS;
	for (const QString& folder : folderPaths) {
		QDir dir(folder);
		QStringList files = dir.entryList(imageFilters, QDir::Files, QDir::Name);
		// Prepend the full path so ThumbnailWidget can load them directly.
		for (QString& f : files)
			f = dir.filePath(f);
		m_folderFrames.append(std::move(files));
	}

	// The slider spans 0 .. maxFrameCount-1 (the longest folder); a folder with no frame at the current
	// index just shows an empty cell (see loadCurrentFrame).
	int maxFrameCount = 0;
	for (const QStringList& frames : std::as_const(m_folderFrames))
		maxFrameCount = qMax(maxFrameCount, static_cast<int>(frames.size()));

	QHBoxLayout* thumbnailRow = new QHBoxLayout();
	// No initial paths, no size required here - the first load will happen on resize
	for (int i = 0; i < folderPaths.size(); ++i)
	{
		m_thumbnailWidgets.push_back(new ThumbnailWidget(QString{}, QString{}, 0, this));
		thumbnailRow->addWidget(m_thumbnailWidgets.back());
	}

	m_slider = new QSlider(Qt::Horizontal, this);
	m_slider->setMinimum(0);
	m_slider->setMaximum(qMax(0, maxFrameCount - 1));
	m_slider->setValue(0);
	m_slider->setPageStep(5);

	m_frameLabel = new QLabel(tr("Frame: 1 / %1").arg(maxFrameCount), this);
	m_frameLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	m_frameLabel->setMinimumWidth(120);

	QHBoxLayout* sliderRow = new QHBoxLayout();
	sliderRow->addWidget(m_slider);
	sliderRow->addWidget(m_frameLabel);

	QVBoxLayout* mainLayout = new QVBoxLayout(this);
	mainLayout->addLayout(thumbnailRow, 1);
	mainLayout->addLayout(sliderRow, 0);

	// Debounce timer: single-shot, fires after DEBOUNCE_MS of slider inactivity.
	m_debounceTimer = new QTimer(this);
	m_debounceTimer->setSingleShot(true);
	connect(m_debounceTimer, &QTimer::timeout, this, &CompareWindow::loadCurrentFrame);

	connect(m_slider, &QSlider::valueChanged, this, [this, maxFrameCount](int value) {
		m_frameLabel->setText(tr("Frame: %1 / %2").arg(value + 1).arg(maxFrameCount));
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
	if (m_debounceTimer)
		startFrameLoadTimer();
}

void CompareWindow::startFrameLoadTimer()
{
	m_debounceTimer->start(DEBOUNCE_MS);
}

void CompareWindow::loadCurrentFrame()
{
	const int frameIndex = m_slider->value();

	QStringList paths;
	paths.reserve(m_folderFrames.size());

	assert(m_thumbnailWidgets.size() == m_folderFrames.size());

	for (qsizetype i = 0; i < m_thumbnailWidgets.size(); ++i)
	{
		ThumbnailWidget* widget = m_thumbnailWidgets[i];
		if (frameIndex < m_folderFrames[i].size())
		{
			const QString framePath = m_folderFrames[i][frameIndex];
			QString caption = QFileInfo(framePath).fileName();
			widget->loadFrame(framePath, caption);
		}
		else
			widget->loadFrame(QString{}, QString{}); // Clear the thumbnail if there's no matching frame index
	}
}
