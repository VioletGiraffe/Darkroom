#include "Windows/FrameViewerWindow.h"
#include "Theme/Theme.h"
#include "UiComponents/ThumbnailWidget.h"
#include "Utils.h"
#include "widgets/layouts/cflowlayout.h"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFileInfo>
#include <QLabel>
#include <QMenu>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QShortcut>
#include <QTimer>
#include <QVBoxLayout>

static constexpr int DEFAULT_THUMBNAIL_SIZE = 200;
static constexpr int MIN_THUMBNAIL_SIZE = 60;
static constexpr int MAX_THUMBNAIL_SIZE = 600;
static constexpr int THUMBNAIL_SIZE_STEP = 20;
static const QString SETTINGS_KEY_THUMBNAIL_SIZE = "frameViewer/thumbnailSize";

FrameViewerWindow::FrameViewerWindow(QWidget* parent)
	: QWidget{ parent, Qt::Window }
{
	setWindowTitle(tr("Frame Viewer"));
	resize(1200, 800);

	m_thumbnailSize = QSettings{}.value(SETTINGS_KEY_THUMBNAIL_SIZE, DEFAULT_THUMBNAIL_SIZE).toInt();

	auto* layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);

	m_instructionLabel = new QLabel(this);
	m_instructionLabel->setAlignment(Qt::AlignCenter);
	m_instructionLabel->setStyleSheet(QStringLiteral("font-size: 16pt; color: %1;").arg(Theme::current().InstructionText));
	m_instructionLabel->hide();
	layout->addWidget(m_instructionLabel);

	m_scrollArea = new QScrollArea(this);
	m_scrollArea->setWidgetResizable(true);
	m_scrollArea->setFrameShape(QFrame::NoFrame);   // the whole client area is this scroll area - nothing to frame off
	m_scrollArea->setFocusPolicy(Qt::StrongFocus);
	m_scrollArea->viewport()->setMouseTracking(true);

	m_thumbnailContainer = new QWidget();
	m_thumbnailContainer->setMouseTracking(true);
	m_thumbnailContainer->setFocusPolicy(Qt::NoFocus);

	m_thumbnailLayout = new CFlowLayout(m_thumbnailContainer);
	m_thumbnailLayout->setSpacing(10);
	m_thumbnailLayout->setContentsMargins(10, 10, 10, 10);

	m_scrollArea->setWidget(m_thumbnailContainer);
	layout->addWidget(m_scrollArea);

	m_refreshDebounceTimer = new QTimer(this);
	m_refreshDebounceTimer->setSingleShot(true);
	m_refreshDebounceTimer->setInterval(100);
	connect(m_refreshDebounceTimer, &QTimer::timeout, this, &FrameViewerWindow::refreshDisplay);

	auto* escShortcut = new QShortcut(Qt::Key_Escape, this);
	connect(escShortcut, &QShortcut::activated, this, &QWidget::close);
}

void FrameViewerWindow::showForFolder(const QString& folderPath)
{
	m_folderPath = folderPath;
	setWindowTitle(folderPath.isEmpty() ? tr("Frame Viewer") : QFileInfo(folderPath).fileName());
	refreshDisplay();
	if (!folderPath.isEmpty())
	{
		show();
		raise();
		activateWindow();
	}
}

void FrameViewerWindow::refreshDisplay()
{
	while (m_thumbnailLayout->count() > 0)
	{
		QLayoutItem* item = m_thumbnailLayout->takeAt(0);
		if (item->widget())
			item->widget()->deleteLater();
		delete item;
	}

	if (m_folderPath.isEmpty())
	{
		showInstruction(tr("Select a video to view its frames"));
		return;
	}

	QDir dir(m_folderPath);
	if (!dir.exists())
	{
		showInstruction(tr("Folder not found: %1").arg(m_folderPath));
		return;
	}

	const QStringList imageFiles = dir.entryList(IMAGE_FILE_FILTERS, QDir::Files, QDir::Name);
	if (imageFiles.isEmpty())
	{
		showInstruction(tr("No images found in this folder"));
		return;
	}

	m_instructionLabel->hide();
	m_scrollArea->show();

	for (const QString& fileName : std::as_const(imageFiles))
	{
		const QString filePath = m_folderPath + "/" + fileName;
		auto* thumbnail = new ThumbnailWidget(filePath, QFileInfo{ filePath }.completeBaseName(), m_thumbnailSize, m_thumbnailContainer);
		connect(thumbnail, &ThumbnailWidget::customContextMenuRequested, this, &FrameViewerWindow::showThumbnailContextMenu);
		thumbnail->setOnMouseWheelCallback([this](int steps) { zoomThumbnails(steps); });
		m_thumbnailLayout->addWidget(thumbnail);
	}

	if (auto* sb = m_scrollArea->verticalScrollBar())
		sb->setValue(0);
}

void FrameViewerWindow::showInstruction(const QString& text)
{
	m_instructionLabel->setText(text);
	m_instructionLabel->show();
	m_scrollArea->hide();
}

void FrameViewerWindow::zoomThumbnails(int steps)
{
	const int current = m_thumbnailSize;
	const int next = qBound(MIN_THUMBNAIL_SIZE, current + steps * THUMBNAIL_SIZE_STEP, MAX_THUMBNAIL_SIZE);
	if (next == current)
		return;

	m_thumbnailSize = next;
	QSettings{}.setValue(SETTINGS_KEY_THUMBNAIL_SIZE, m_thumbnailSize);
	m_refreshDebounceTimer->start(); // rebuild once the wheel settles
}

void FrameViewerWindow::showThumbnailContextMenu(const QPoint& pos)
{
	auto* senderWidget = dynamic_cast<ThumbnailWidget*>(sender());
	if (!senderWidget)
		return;

	QMenu menu;
	menu.addAction(tr("Open in Explorer"), [senderWidget] {
		openInExplorer(senderWidget->filePath());
	});
	menu.addAction(tr("Copy file path"), [senderWidget] {
		QApplication::clipboard()->setText(QDir::toNativeSeparators(senderWidget->filePath()));
	});
	menu.exec(senderWidget->mapToGlobal(pos));
	clearStuckHoverIfCursorLeft(senderWidget);  // else the popup grab leaves #framedThumbnail:hover stuck on
}
