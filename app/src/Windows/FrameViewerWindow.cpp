#include "Windows/FrameViewerWindow.h"
#include "Theme/Style.h"
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

	_thumbnailSize = QSettings{}.value(SETTINGS_KEY_THUMBNAIL_SIZE, DEFAULT_THUMBNAIL_SIZE).toInt();

	auto* layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);

	_instructionLabel = new QLabel(this);
	_instructionLabel->setAlignment(Qt::AlignCenter);
	Style::applyThemedSheet(_instructionLabel, [] {
		return QStringLiteral("font-size: 16pt; color: %1;").arg(Theme::current().InstructionText);
	});
	_instructionLabel->hide();
	layout->addWidget(_instructionLabel);

	_scrollArea = new QScrollArea(this);
	_scrollArea->setWidgetResizable(true);
	_scrollArea->setFrameShape(QFrame::NoFrame);   // the whole client area is this scroll area - nothing to frame off
	_scrollArea->setFocusPolicy(Qt::StrongFocus);
	_scrollArea->viewport()->setMouseTracking(true);

	_thumbnailContainer = new QWidget();
	_thumbnailContainer->setMouseTracking(true);
	_thumbnailContainer->setFocusPolicy(Qt::NoFocus);

	_thumbnailLayout = new CFlowLayout(_thumbnailContainer);
	_thumbnailLayout->setSpacing(10);
	_thumbnailLayout->setContentsMargins(10, 10, 10, 10);

	_scrollArea->setWidget(_thumbnailContainer);
	layout->addWidget(_scrollArea);

	_refreshDebounceTimer = new QTimer(this);
	_refreshDebounceTimer->setSingleShot(true);
	_refreshDebounceTimer->setInterval(100);
	connect(_refreshDebounceTimer, &QTimer::timeout, this, &FrameViewerWindow::refreshDisplay);

	auto* escShortcut = new QShortcut(Qt::Key_Escape, this);
	connect(escShortcut, &QShortcut::activated, this, &QWidget::close);
}

void FrameViewerWindow::showForFolder(const QString& folderPath, const QString& title)
{
	_folderPath = folderPath;
	setWindowTitle(title.isEmpty() ? tr("Frame Viewer") : title);
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
	while (_thumbnailLayout->count() > 0)
	{
		QLayoutItem* item = _thumbnailLayout->takeAt(0);
		if (item->widget())
			item->widget()->deleteLater();
		delete item;
	}

	if (_folderPath.isEmpty())
	{
		showInstruction(tr("Select a video to view its frames"));
		return;
	}

	QDir dir(_folderPath);
	if (!dir.exists())
	{
		showInstruction(tr("Folder not found: %1").arg(_folderPath));
		return;
	}

	const QStringList imageFiles = listFrameImageFiles(dir);
	if (imageFiles.isEmpty())
	{
		showInstruction(tr("No images found in this folder"));
		return;
	}

	_instructionLabel->hide();
	_scrollArea->show();

	for (const QString& fileName : std::as_const(imageFiles))
	{
		const QString filePath = _folderPath + "/" + fileName;
		auto* thumbnail = new ThumbnailWidget(filePath, QFileInfo{ filePath }.completeBaseName(), _thumbnailSize, _thumbnailContainer);
		connect(thumbnail, &ThumbnailWidget::customContextMenuRequested, this, &FrameViewerWindow::showThumbnailContextMenu);
		thumbnail->setOnMouseWheelCallback([this](int steps) { zoomThumbnails(steps); });
		_thumbnailLayout->addWidget(thumbnail);
	}

	if (auto* sb = _scrollArea->verticalScrollBar())
		sb->setValue(0);
}

void FrameViewerWindow::showInstruction(const QString& text)
{
	_instructionLabel->setText(text);
	_instructionLabel->show();
	_scrollArea->hide();
}

void FrameViewerWindow::zoomThumbnails(int steps)
{
	const int current = _thumbnailSize;
	const int next = qBound(MIN_THUMBNAIL_SIZE, current + steps * THUMBNAIL_SIZE_STEP, MAX_THUMBNAIL_SIZE);
	if (next == current)
		return;

	_thumbnailSize = next;
	QSettings{}.setValue(SETTINGS_KEY_THUMBNAIL_SIZE, _thumbnailSize);
	_refreshDebounceTimer->start(); // rebuild once the wheel settles
}

void FrameViewerWindow::showThumbnailContextMenu(const QPoint& pos)
{
	auto* senderWidget = dynamic_cast<ThumbnailWidget*>(sender());
	if (!senderWidget)
		return;

	QMenu menu;
	menu.addAction(revealInFileManagerActionText(), [senderWidget, this] {
		if (const QString path = senderWidget->filePath(); !revealInFileManager(path))
			reportMissingFile(this, path);
	});
	menu.addAction(tr("Copy file path"), [senderWidget] {
		QApplication::clipboard()->setText(QDir::toNativeSeparators(senderWidget->filePath()));
	});
	menu.exec(senderWidget->mapToGlobal(pos));

	clearStuckHoverIfCursorLeft(senderWidget);  // else the popup grab leaves #framedThumbnail:hover stuck on
}
