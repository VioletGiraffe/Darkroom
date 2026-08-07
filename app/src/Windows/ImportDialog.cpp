#include "Windows/ImportDialog.h"
#include "Core/Catalog.h"
#include "Core/LabelId.h"
#include "Core/Library.h"
#include "Ffmpeg.h"
#include "UiComponents/ContentWidthListWidget.h"
#include "UiComponents/DragGestureHelper.h"
#include "UiComponents/LabelMimeType.h"
#include "UiComponents/LabelRowDelegate.h"
#include "UiComponents/LabelVisuals.h"
#include "UiComponents/MediaGrid.h"
#include "UiComponents/PreviewFrameCountCombo.h"
#include "UiComponents/SegmentedToggle.h"
#include "Settings.h"
#include "Shortcuts.h"
#include "Windows/ImportExecution.h"
#include "Windows/LabelManagement.h"
#include "Windows/MediaItemManagement.h"
#include "Windows/SourceRelocation.h"
#include "Theme/Icons.h"
#include "Theme/Theme.h"
#include "Utils.h"
#include "UiComponents/MediaItemWidget.h"
#include "Windows/PhotoCompareWindow.h"
#include "Windows/VideoPlayerWindow.h"

#include "assert/advanced_assert.h"
#include "dialogs/messagebox.h"
#include "threading/cinterruptablethread.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QColorDialog>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QProgressDialog>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
#include <QTimer>
#include <QThread>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <atomic>

namespace {

constexpr int kLabelIdRole = Qt::UserRole;

const QString STAGED_CARD_IMAGE_HEIGHT_KEY = QStringLiteral("importDialog/cardImageHeight");
const QString STAGED_PREVIEW_FRAME_COUNT_KEY = QStringLiteral("importDialog/previewFrameCount");
constexpr int DEFAULT_STAGED_CARD_IMAGE_HEIGHT = 120;
constexpr int MIN_STAGED_CARD_IMAGE_HEIGHT = 60;
constexpr int MAX_STAGED_CARD_IMAGE_HEIGHT = 360;
constexpr int STAGED_CARD_IMAGE_HEIGHT_STEP = 20;

constexpr int LABEL_LIST_MAX_WIDTH = 300;

// Each ffmpeg process is internally threaded; keep concurrent processes low.
constexpr int PREVIEW_EXTRACTION_CONCURRENCY = 2;
constexpr int TEMP_PREVIEW_DELETE_TIMEOUT_MS = 1000;
constexpr int TEMP_PREVIEW_DELETE_RETRY_MS = 20;

int stagedCardImageHeight()
{
	return QSettings{}.value(STAGED_CARD_IMAGE_HEIGHT_KEY, DEFAULT_STAGED_CARD_IMAGE_HEIGHT).toInt();
}

QString uniqueTempPreviewDir()
{
	return QDir::tempPath() + "/darkroom_import/" + QUuid::createUuid().toString(QUuid::Id128);
}

bool removeTempPreviewDirWithRetry(const QString& path)
{
	// QDir("") addresses the working directory; photos have no temporary directory.
	if (path.isEmpty())
		return true;

	QElapsedTimer timeout;
	timeout.start();
	for (;;)
	{
		if (deleteFolderRecursivelyIfPresent(path))
			return true;
		if (timeout.elapsed() >= TEMP_PREVIEW_DELETE_TIMEOUT_MS)
			return false;
		QThread::msleep(TEMP_PREVIEW_DELETE_RETRY_MS);
	}
}

std::vector<Ffmpeg::PreviewResult> generatePreviewFramesInteractively(
	const std::vector<Ffmpeg::PreviewJob>& jobs, int frameCount, QWidget* parent)
{
	if (jobs.empty())
		return {};

	QProgressDialog progress(QObject::tr("Examining video %1/%2...").arg(0).arg(jobs.size()), QObject::tr("Cancel"),
		0, static_cast<int>(jobs.size()), parent);
	progress.setWindowTitle(QObject::tr("Staging"));
	progress.setModal(true);
	progress.setMinimumDuration(0);
	// Keep the dialog open until cancelled ffmpeg processes have been reaped.
	progress.setAutoClose(false);
	progress.setAutoReset(false);

	// results is written by this worker and read only after join().
	std::vector<Ffmpeg::PreviewResult> results;
	CInterruptableThread previewThread{ "preview-extraction" };

	QObject::connect(&progress, &QProgressDialog::canceled, parent, [&] {
		previewThread.requestCancellation();
		progress.setLabelText(QObject::tr("Cancelling..."));
	});

	previewThread.start([&](const std::atomic<bool>& cancelled) {
		results = Ffmpeg::generatePreviewFrames(jobs, frameCount, PREVIEW_EXTRACTION_CONCURRENCY, cancelled,
			[&](int done, int total, Ffmpeg::Phase phase) {
				QMetaObject::invokeMethod(&progress, [&progress, &previewThread, done, total, phase] {
					if (previewThread.cancellationRequested())
						return;
					if (phase == Ffmpeg::Phase::Probing)
					{
						progress.setLabelText(QObject::tr("Examining video %1/%2...").arg(done).arg(total));
						return;
					}
					progress.setValue(done);
					progress.setLabelText(QObject::tr("Generating preview %1/%2...").arg(done).arg(total));
				}, Qt::QueuedConnection);
			});
		QMetaObject::invokeMethod(&progress, [&progress] { progress.accept(); }, Qt::QueuedConnection);
	});

	progress.exec();

	// Cover completion, Cancel, Esc, and window close before joining the worker.
	previewThread.requestCancellation();
	previewThread.join();
	return results;
}

// suggestedLabelName preserves the dropped folder hierarchy as hyphen-joined components.
struct StagedFile
{
	QString path;
	QString suggestedLabelName;
};

QList<StagedFile> flattenToSupportedMediaFiles(const QStringList& paths)
{
	QList<StagedFile> files;
	for (const QString& path : paths)
	{
		if (QFileInfo(path).isDir())
		{
			// Use the dropped folder's parent so its own name remains the first label component.
			const QDir base(QFileInfo(QDir::cleanPath(path)).absolutePath());
			for (const QString& file : collectFilesInDirectory(path, /*recursive=*/true, isSupportedMediaFile))
				files.append({ file, base.relativeFilePath(QFileInfo(file).absolutePath()).replace('/', '-') });
		}
		else if (isSupportedMediaFile(path))
			files.append({ path, {} });
	}
	return files;
}

using RelocateMode = SourceRelocation::Mode;

// A matching id elsewhere may be a collision, so success requires the expected destination.
[[nodiscard]] bool isTrackedUnderLabel(const Catalog& catalog, const MediaId& id, const QString& labelId)
{
	const QString storageFolder = catalog.storageFolderForLabel(labelIdFromString(labelId));
	if (storageFolder.isEmpty())
		return false;
	const QString expectedFolder = storageFolder + "/" + Catalog::frameFolderName(QFileInfo(id.name()).completeBaseName(), id);
	return QString::compare(catalog.folderForMediaItem(id), expectedFolder, Qt::CaseInsensitive) == 0;
}

} // namespace

ImportDialog::ImportDialog(Library& library, const QString& suggestedRelocateFolder, QWidget* parent)
	: QDialog(parent)
	, _library(library)
{
	setWindowTitle(tr("Import"));
	// Qt::Window gives this workspace its own taskbar and Alt-Tab presence.
	setWindowFlags(Qt::Window | Qt::WindowTitleHint | Qt::WindowSystemMenuHint | Qt::WindowMinMaxButtonsHint | Qt::WindowCloseButtonHint);
	setWindowModality(Qt::ApplicationModal);
	setAcceptDrops(true);

	QVBoxLayout* outerLayout = new QVBoxLayout(this);

	QSettings relocateSettings;
	_relocateModeCombo = new QComboBox(this);
	_relocateModeCombo->addItem(tr("Leave source file in place (photos: reference, never touched)"), int(RelocateMode::LeaveInPlace));
	_relocateModeCombo->addItem(tr("Copy source file to:"), int(RelocateMode::Copy));
	_relocateModeCombo->addItem(tr("Move source file to:"), int(RelocateMode::Move));
	_relocateModeCombo->setPlaceholderText(tr("Choose how to handle source files..."));
	_relocateModeCombo->setCurrentIndex(-1);

	QString relocateFolder = relocateSettings.value("importDialog/relocateFolder").toString();
	if (relocateFolder.isEmpty())
		relocateFolder = suggestedRelocateFolder;
	_relocateFolderEdit = new QLineEdit(relocateFolder, this);
	QPushButton* relocateBrowseButton = new QPushButton(tr("Browse..."), this);

	const auto updateRelocateRowEnabled = [this, relocateBrowseButton] {
		const auto modeData = _relocateModeCombo->currentData();
		const bool enabled = modeData.isValid() && modeData.toInt() != int(RelocateMode::LeaveInPlace);
		_relocateFolderEdit->setEnabled(enabled);
		relocateBrowseButton->setEnabled(enabled);
	};
	updateRelocateRowEnabled();
	connect(_relocateModeCombo, &QComboBox::currentIndexChanged, this, updateRelocateRowEnabled);

	connect(relocateBrowseButton, &QPushButton::clicked, this, [this] {
		const QString path = QFileDialog::getExistingDirectory(this, tr("Select destination folder"), _relocateFolderEdit->text());
		if (!path.isEmpty())
			_relocateFolderEdit->setText(QDir::toNativeSeparators(path));
	});

	QHBoxLayout* relocateRow = new QHBoxLayout;
	relocateRow->addWidget(new QLabel(tr("Source file:"), this));
	relocateRow->addWidget(_relocateModeCombo);
	relocateRow->addWidget(_relocateFolderEdit, 1);
	relocateRow->addWidget(relocateBrowseButton);
	outerLayout->addLayout(relocateRow);

	_splitter = new QSplitter(Qt::Horizontal, this);
	outerLayout->addWidget(_splitter, 1);

	QWidget* labelPane = new QWidget();
	QVBoxLayout* labelPaneLayout = new QVBoxLayout(labelPane);
	labelPaneLayout->setContentsMargins(0, 0, 0, 0);
	labelPaneLayout->addWidget(new QLabel(tr("Labels")));

	_labelList = new ContentWidthListWidget();
	_labelList->setSelectionMode(QAbstractItemView::NoSelection);
	_labelList->setFrameShape(QFrame::NoFrame);
	_labelList->setMaximumWidth(LABEL_LIST_MAX_WIDTH);
	_labelList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	_labelList->setItemDelegate(new LabelRowDelegate(_labelList));
	_labelList->setMouseTracking(true);
	new ListRowDragFilter(_labelList, [](const QListWidgetItem* item) {
		auto* mime = new QMimeData();
		mime->setData(LabelMimeType, item->data(kLabelIdRole).toString().toUtf8());
		return mime;
	});
	_labelList->setContextMenuPolicy(Qt::CustomContextMenu);
	connect(_labelList, &QListWidget::customContextMenuRequested, this, &ImportDialog::showLabelListContextMenu);
	labelPaneLayout->addWidget(_labelList, 1);

	QPushButton* addLabelButton = new QPushButton(tr("Create label"));
	addLabelButton->setObjectName("addLabelButton");
	addLabelButton->setIcon(Theme::tintedIcon(QStringLiteral(":/UI/icon_plus.svg"), &Theme::ThemeColors::TextPrimary));
	addLabelButton->setShortcut(QKeySequence(Shortcuts::CreateLabel));
	addLabelButton->setToolTip(tr("Create a new label (%1)").arg(addLabelButton->shortcut().toString(QKeySequence::NativeText)));
	connect(addLabelButton, &QPushButton::clicked, this, [this] {
		bool ok = false;
		const QString name = QInputDialog::getText(this, tr("New Label"), tr("Label name:"), QLineEdit::Normal, QString{}, &ok);
		if (!ok)
			return;
		if (const char* error = Catalog::labelNameValidationError(name))
		{
			QMessageBox::warning(this, tr("New Label"), tr(error));
			return;
		}
		if (!findLabelIdByName(name, {}).isEmpty())
		{
			QMessageBox::information(this, tr("New Label"), tr("A label named \"%1\" already exists.").arg(name));
			return;
		}
		addProvisionalLabel(name);
		refreshLabelList();
	});
	labelPaneLayout->addWidget(addLabelButton);

	_splitter->addWidget(labelPane);

	QWidget* stagedPane = new QWidget();
	QVBoxLayout* stagedPaneLayout = new QVBoxLayout(stagedPane);
	stagedPaneLayout->setContentsMargins(0, 0, 0, 0);

	const int mainWindowFrameCount = QSettings{}.value(Settings::PreviewFrameCount, Defaults::PreviewFrameCount).toInt();
	const int savedStagedFrameCount = QSettings{}.value(STAGED_PREVIEW_FRAME_COUNT_KEY, mainWindowFrameCount).toInt();
	_mediaTypeFilter = new SegmentedToggle({ tr("All"), tr("Videos"), tr("Photos") });
	connect(_mediaTypeFilter, &SegmentedToggle::currentChanged, this, &ImportDialog::applyStagedMediaTypeFilter);
	_previewFrameCountCombo = new PreviewFrameCountCombo(savedStagedFrameCount);
	QSettings{}.setValue(STAGED_PREVIEW_FRAME_COUNT_KEY, _previewFrameCountCombo->frameCount());
	connect(_previewFrameCountCombo, &QComboBox::currentIndexChanged, this, &ImportDialog::stagedPreviewFrameCountChanged);
	QHBoxLayout* stagedToolbar = new QHBoxLayout;
	stagedToolbar->addWidget(_mediaTypeFilter, 0, Qt::AlignVCenter);
	stagedToolbar->addStretch(1);
	stagedToolbar->addWidget(_previewFrameCountCombo, 0, Qt::AlignVCenter);
	stagedPaneLayout->addLayout(stagedToolbar);

	_stagedGrid = new MediaGrid();
	_stagedGrid->setStyleSheet(QStringLiteral("QListWidget::item:selected { background-color: %1; }").arg(Theme::current().AccentBg));

	_stagedCardZoomDebounce = new QTimer(this);
	_stagedCardZoomDebounce->setSingleShot(true);
	_stagedCardZoomDebounce->setInterval(80);
	connect(_stagedCardZoomDebounce, &QTimer::timeout, this, &ImportDialog::rebuildAllStagedCards);
	stagedPaneLayout->addWidget(_stagedGrid, 1);

	auto* renameStagedAction = new QAction(tr("Rename..."), this);
	renameStagedAction->setShortcut(QKeySequence(Shortcuts::Rename));
	renameStagedAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
	connect(renameStagedAction, &QAction::triggered, this, [this] {
		const std::vector<MediaId> ids = selectedStagedIds();
		if (ids.size() == 1)
			renameStagedItem(ids.front());
	});
	_stagedGrid->addAction(renameStagedAction);

	auto* removeStagedAction = new QAction(tr("Remove from staging"), this);
	removeStagedAction->setShortcut(QKeySequence(Shortcuts::RemoveFromList));
	removeStagedAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
	connect(removeStagedAction, &QAction::triggered, this, [this] { removeStagedItems(selectedStagedIds()); });
	_stagedGrid->addAction(removeStagedAction);

	auto* deleteStagedAction = new QAction(tr("Delete source file(s)"), this);
	deleteStagedAction->setShortcut(QKeySequence(Shortcuts::DeleteFile));
	deleteStagedAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
	connect(deleteStagedAction, &QAction::triggered, this, [this] { deleteStagedSourceFiles(selectedStagedIds()); });
	_stagedGrid->addAction(deleteStagedAction);

	_splitter->addWidget(stagedPane);

	_splitter->setStretchFactor(0, 0);
	_splitter->setStretchFactor(1, 1);
	_splitter->setCollapsible(0, false);

	QLabel* instructions = new QLabel(
		tr("Drop video or image files here to stage them, then drag labels from the list onto a card to tag it. "
		   "Dropping a folder stages the media under it; click \"Suggest labels\" to apply labels derived from folder names. Right-click a "
		   "label to rename, recolor, or remove it before importing. "
		   "Double-click a card to preview; right-click for more options. \"Import\" imports every labeled card "
		   "and clears it from staging."), this);
	instructions->setWordWrap(true);
	instructions->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::current().MutedText));
	outerLayout->addWidget(instructions);

	QHBoxLayout* footer = new QHBoxLayout;
	footer->addStretch(1);
	QPushButton* suggestLabelsButton = new QPushButton(tr("Suggest labels"), this);
	connect(suggestLabelsButton, &QPushButton::clicked, this, &ImportDialog::suggestLabels);
	footer->addWidget(suggestLabelsButton);
	QPushButton* importButton = new QPushButton(tr("Import"), this);
	connect(importButton, &QPushButton::clicked, this, &ImportDialog::runImport);
	footer->addWidget(importButton);
	outerLayout->addLayout(footer);

	refreshLabelList();

	if (!restoreWindowGeometry(this, "importDialog"))
	{
		resize(1200, 800);
		setWindowState(Qt::WindowMaximized);
	}
	const QByteArray splitterState = QSettings{}.value("importDialog/splitter").toByteArray();
	if (!splitterState.isEmpty())
		_splitter->restoreState(splitterState);
}

ImportDialog::~ImportDialog()
{
	saveWindowGeometry(this, "importDialog");
	QSettings{}.setValue("importDialog/splitter", _splitter->saveState());
	QSettings{}.setValue("importDialog/relocateFolder", _relocateFolderEdit->text());

	for (const StagedEntry& entry : std::as_const(_staged))
		assert_r(removeTempPreviewDirWithRetry(entry.tempPreviewDir));
}

void ImportDialog::reject()
{
	const bool anyHiddenItems = std::any_of(_staged.constBegin(), _staged.constEnd(),
		[](const StagedEntry& entry) { return entry.item->isHidden(); });
	if (anyHiddenItems)
	{
		_mediaTypeFilter->setCurrentIndex(0);
		applyStagedMediaTypeFilter();
		return;
	}

	QDialog::reject();
}

void ImportDialog::dragEnterEvent(QDragEnterEvent* event)
{
	if (hasSupportedPaths(event->mimeData()))
		event->acceptProposedAction();
}

void ImportDialog::dropEvent(QDropEvent* event)
{
	QStringList paths = supportedPaths(event->mimeData());
	if (!paths.isEmpty())
	{
		raise();
		activateWindow();
		QMetaObject::invokeMethod(this, [this, paths=std::move(paths)] {
			stageMediaItems(paths);
		}, Qt::QueuedConnection);
	}
}

void ImportDialog::refreshLabelList()
{
	_labelOptions.clear();
	for (const LabelOption& provisional : _provisionalLabels)
		_labelOptions.push_back(provisional);
	for (const Catalog::Label& label : _library.catalog().allLabels())
		if (!label.isVirtual())
			_labelOptions.push_back(LabelOption{ toString(label.id), label.displayName, label.color });

	_labelList->clear();
	for (const LabelOption& option : _labelOptions)
	{
		auto* item = new QListWidgetItem(
			option.provisional ? tr("%1  (new)").arg(option.displayName) : option.displayName, _labelList);
		item->setData(kLabelIdRole, option.id);
		item->setData(LabelRowDelegate::SwatchColorRole, LabelRowDelegate::swatchColor(option.color));
		if (option.provisional)
		{
			QFont font = item->font();
			font.setItalic(true);
			item->setFont(font);
			item->setToolTip(tr("New label - created when you click Import."));
		}
	}

	_labelList->updateGeometry();
}

const ImportDialog::LabelOption* ImportDialog::findLabelOption(const QString& id) const
{
	for (const LabelOption& option : _labelOptions)
		if (option.id == id)
			return &option;
	return nullptr;
}

bool ImportDialog::isProvisionalId(const QString& id)
{
	return id.startsWith(QLatin1String("new:"));
}

QString ImportDialog::findLabelIdByName(const QString& name, const QString& excludeId) const
{
	for (const LabelOption& option : _labelOptions)
		if (option.id != excludeId && option.displayName.compare(name, Qt::CaseInsensitive) == 0)
			return option.id;
	return {};
}

QString ImportDialog::addProvisionalLabel(const QString& name)
{
	LabelOption option;
	option.id = QStringLiteral("new:%1").arg(_provisionalSeq++);
	option.displayName = name;
	option.color = Catalog::randomLabelColor();
	option.provisional = true;
	_provisionalLabels.push_back(option);
	_labelOptions.push_back(option);
	return option.id;
}

QString ImportDialog::ensureLabelForFolderName(const QString& name)
{
	if (name.isEmpty())
		return {};
	const QString existing = findLabelIdByName(name, {});
	return existing.isEmpty() ? addProvisionalLabel(name) : existing;
}

void ImportDialog::showLabelListContextMenu(const QPoint& pos)
{
	QListWidgetItem* item = _labelList->itemAt(pos);
	if (!item)
		return;
	const QString labelId = item->data(kLabelIdRole).toString();

	QMenu menu(this);
	if (isProvisionalId(labelId))
	{
		menu.addAction(tr("Rename..."), this, [this, labelId] { renameProvisionalLabel(labelId); });
		menu.addAction(tr("Set color..."), this, [this, labelId] { setProvisionalLabelColor(labelId); });
		menu.addAction(tr("Delete"), this, [this, labelId] { deleteProvisionalLabel(labelId); });
	}
	else
	{
		menu.addAction(tr("Rename..."), this, [this] {
			QMessageBox::information(this, tr("Rename label"),
				tr("This label already exists in the catalog, you can rename it in the main window. All changes are "
				   "temporary until \"Import\" is clicked. You can create a new label with your desired name; or "
				   "assign the existing label and rename it after import runs."));
		});
	}
	menu.exec(_labelList->viewport()->mapToGlobal(pos));
}

void ImportDialog::renameProvisionalLabel(const QString& provisionalId)
{
	const auto option = std::find_if(_provisionalLabels.begin(), _provisionalLabels.end(),
		[&](const LabelOption& o) { return o.id == provisionalId; });
	if (option == _provisionalLabels.end())
		return;

	bool ok = false;
	const QString newName = QInputDialog::getText(this, tr("Rename label"), tr("New name:"),
		QLineEdit::Normal, option->displayName, &ok);
	if (!ok || newName == option->displayName)
		return;
	if (const char* error = Catalog::labelNameValidationError(newName))
	{
		QMessageBox::warning(this, tr("Rename label"), tr(error));
		return;
	}

	if (const QString clashId = findLabelIdByName(newName, provisionalId); !clashId.isEmpty())
	{
		if (QMessageBox::question(this, tr("Merge labels"), tr("A label named \"%1\" already exists. Merge into it?").arg(newName)) == QMessageBox::Yes)
			mergeProvisionalInto(provisionalId, clashId);
		return;
	}

	option->displayName = newName;
	refreshLabelList();
	updateAllCardLabelDots();
}

void ImportDialog::setProvisionalLabelColor(const QString& provisionalId)
{
	const auto option = std::find_if(_provisionalLabels.begin(), _provisionalLabels.end(),
		[&](const LabelOption& o) { return o.id == provisionalId; });
	if (option == _provisionalLabels.end())
		return;

	const QColor initial = option->color.isEmpty() ? QColor(Qt::white) : QColor(option->color);
	const QColor chosen = QColorDialog::getColor(initial, this, tr("Label color"));
	if (!chosen.isValid())
		return;

	option->color = chosen.name();
	refreshLabelList();
	updateAllCardLabelDots();
}

bool ImportDialog::remapStagedLabelIds(const QHash<QString, QString>& mapping)
{
	bool anyDropped = false;
	for (auto it = _staged.begin(); it != _staged.end(); ++it)
	{
		QStringList remapped;
		for (const QString& labelId : std::as_const(it->pendingLabelIds))
		{
			const QString mapped = mapping.value(labelId, labelId);
			if (mapped.isEmpty())
				anyDropped = true;
			else if (!remapped.contains(mapped))
				remapped << mapped;
		}
		if (remapped != it->pendingLabelIds)
		{
			it->pendingLabelIds = remapped;
			updateCardLabelDots(it.key());
		}
	}
	return anyDropped;
}

void ImportDialog::deleteProvisionalLabel(const QString& provisionalId)
{
	remapStagedLabelIds({ { provisionalId, QString{} } });

	_provisionalLabels.erase(std::remove_if(_provisionalLabels.begin(), _provisionalLabels.end(),
		[&](const LabelOption& o) { return o.id == provisionalId; }), _provisionalLabels.end());
	refreshLabelList();
}

void ImportDialog::mergeProvisionalInto(const QString& provisionalId, const QString& targetId)
{
	remapStagedLabelIds({ { provisionalId, targetId } });

	_provisionalLabels.erase(std::remove_if(_provisionalLabels.begin(), _provisionalLabels.end(),
		[&](const LabelOption& o) { return o.id == provisionalId; }), _provisionalLabels.end());
	refreshLabelList();
}

void ImportDialog::updateAllCardLabelDots()
{
	for (auto it = _staged.constBegin(); it != _staged.constEnd(); ++it)
		updateCardLabelDots(it.key());
}

void ImportDialog::addToStaging(const QStringList& paths)
{
	// stageMediaItems() may open a modal progress dialog, so wait for this dialog's event loop.
	QMetaObject::invokeMethod(this, [this, paths] { stageMediaItems(paths); }, Qt::QueuedConnection);
}

MediaItemWidget* ImportDialog::buildStagedCard(const MediaId& id, const QString& path, const QString& tempPreviewDir, qint64 durationMs)
{
	const int imageHeight = stagedCardImageHeight();
	QSize canvasSize{ imageHeight, imageHeight };
	const bool isPhoto = isSupportedImageFile(path);
	QStringList previewPaths{ path };
	if (!isPhoto)
	{
		const int frameCount = stagedPreviewFrameCount();
		canvasSize.setWidth(MediaItemWidget::videoCanvasWidthForTiling(imageHeight, frameCount, _stagedGrid->spacing()));
		const QDir previewDir(tempPreviewDir);
		previewPaths = pickEvenlySpacedFrames(previewDir, listFrameImageFiles(previewDir), frameCount);
	}

	auto* card = new MediaItemWidget(
		canvasSize,
		previewPaths, QFileInfo(path).fileName(),
		id,
		/*inBest*/ false,
		[this, id] {
			auto it = _staged.find(id);
			if (it != _staged.end())
				it->pendingBest = !it->pendingBest;
		},
		[this, id] { previewStagedItem(id); },
		[this, id](QPoint globalPos) { showStagedCardContextMenu(id, globalPos); },
		/* dynamicSizeHint */ false,
		/* film strip */ !isPhoto
	);

	card->setOnLabelDropped([this, id](const QString& labelId) {
		for (const MediaId& target : effectiveStagedSelection(id))
		{
			auto it = _staged.find(target);
			if (it != _staged.end() && !it->pendingLabelIds.contains(labelId))
				it->pendingLabelIds << labelId;
			updateCardLabelDots(target);
		}
	});

	card->setDuration(durationMs);
	card->setOnMouseWheelCallback([this](int steps) { zoomStagedCards(steps); });

	return card;
}

bool ImportDialog::stagedItemMatchesMediaTypeFilter(const QString& path) const
{
	const int filterIndex = _mediaTypeFilter->currentIndex();
	if (filterIndex == 1)
		return !isSupportedImageFile(path);
	if (filterIndex == 2)
		return isSupportedImageFile(path);
	return true;
}

void ImportDialog::applyStagedMediaTypeFilter()
{
	for (StagedEntry& entry : _staged)
	{
		const bool hidden = !stagedItemMatchesMediaTypeFilter(entry.path);
		entry.item->setHidden(hidden);
		if (hidden)
			entry.item->setSelected(false);
	}
}

void ImportDialog::stageMediaItems(const QStringList& paths)
{
	const QList<StagedFile> mediaFiles = flattenToSupportedMediaFiles(paths);
	if (mediaFiles.isEmpty())
		return;

	QHash<QString, QString> suggestedLabelNameByPath;
	for (const StagedFile& file : mediaFiles)
		if (!file.suggestedLabelName.isEmpty())
			suggestedLabelNameByPath.insert(file.path, file.suggestedLabelName);

	// MediaId is only a name/size gate; byte-compare matches before treating one as a duplicate.
	QHash<MediaId, QString> stagedPathById;
	for (auto it = _staged.constBegin(); it != _staged.constEnd(); ++it)
		stagedPathById.insert(it.key(), it->path);

	QStringList newPaths;
	QStringList collisionLines;
	for (const StagedFile& file : mediaFiles)
	{
		const QString& path = file.path;
		const MediaId id = MediaId::fromFile(path);
		const QString existingPath = stagedPathById.value(id);
		if (existingPath.isEmpty())
		{
			stagedPathById.insert(id, path);
			newPaths << path;
		}
		else if (QFileInfo(path) != QFileInfo(existingPath) && !filesAreIdentical(path, existingPath))
			collisionLines << tr("%1\n    collides with %2").arg(QDir::toNativeSeparators(path), QDir::toNativeSeparators(existingPath));
	}

	if (!collisionLines.isEmpty())
		MessageBox::notice(this, tr("Name collision"),
			tr("Not staged - same name and size as an already staged file, but different content. "
			   "Only one item per name+size can be tracked; rename the file if both are wanted."),
			collisionLines.join("\n\n"));

	if (newPaths.isEmpty())
		return;

	// Photos use content identity; videos use their name/size MediaId.
	QStringList videoPaths;
	QStringList photoPaths;
	QStringList duplicateLines;
	for (const QString& path : newPaths)
	{
		const bool isPhoto = isSupportedImageFile(path);
		const QString existingPath = isPhoto
			? _library.catalog().findPhotoBySameContent(path)
			: _library.catalog().sourcePathForMediaItem(MediaId::fromFile(path));
		if (!existingPath.isEmpty())
			duplicateLines << tr("%1\n    is already imported as %2").arg(QDir::toNativeSeparators(path), QDir::toNativeSeparators(existingPath));
		else
			(isPhoto ? photoPaths : videoPaths) << path;
	}

	if (!duplicateLines.isEmpty())
		MessageBox::notice(this, tr("Already imported"), tr("Not staged - already in the library:"),
			duplicateLines.join("\n\n"), QMessageBox::Information);

	const int mainWindowFrameCount = QSettings{}.value(Settings::PreviewFrameCount, Defaults::PreviewFrameCount).toInt();
	const int extractionFrameCount = std::max(stagedPreviewFrameCount(), mainWindowFrameCount);

	const auto stageCard = [this, &suggestedLabelNameByPath](const QString& path, const QString& tempPreviewDir, qint64 durationMs = -1) {
		const MediaId id = MediaId::fromFile(path);
		auto* card = buildStagedCard(id, path, tempPreviewDir, durationMs);

		auto* item = new QListWidgetItem();
		item->setSizeHint(card->sizeHint());
		_stagedGrid->addItem(item);
		_stagedGrid->setItemWidget(item, card);

		_staged.insert(id, StagedEntry{
			path, tempPreviewDir, suggestedLabelNameByPath.value(path), durationMs, /*pendingBest*/ false, /*pendingLabelIds*/ {}, item });
		item->setHidden(!stagedItemMatchesMediaTypeFilter(path));
	};

	for (const QString& path : photoPaths)
		stageCard(path, /*tempPreviewDir*/ {});

	if (videoPaths.isEmpty())
		return;

	std::vector<Ffmpeg::PreviewJob> jobs;
	jobs.reserve(videoPaths.size());
	for (const QString& path : videoPaths)
		jobs.push_back({ path, uniqueTempPreviewDir() });

	const std::vector<Ffmpeg::PreviewResult> results = generatePreviewFramesInteractively(jobs, extractionFrameCount, this);

	for (size_t i = 0; i < jobs.size(); ++i)
	{
		// Cancelled jobs never become staged entries that could own their scratch directory.
		if (results[i].status == Ffmpeg::PreviewResult::Status::Cancelled)
			assert_r(removeTempPreviewDirWithRetry(jobs[i].destinationFolder));
		else
			stageCard(jobs[i].videoFilePath, jobs[i].destinationFolder, results[i].durationMs);
	}
}

void ImportDialog::suggestLabels()
{
	bool hadSuggestions = false;
	for (auto it = _staged.begin(); it != _staged.end(); ++it)
	{
		if (it->suggestedLabelName.isEmpty())
			continue;

		hadSuggestions = true;
		const QString labelId = ensureLabelForFolderName(it->suggestedLabelName);
		it->suggestedLabelName.clear();
		if (!labelId.isEmpty() && !it->pendingLabelIds.contains(labelId))
			it->pendingLabelIds << labelId;
	}

	if (!hadSuggestions)
		return;

	refreshLabelList();
	updateAllCardLabelDots();
}

void ImportDialog::unstage(const MediaId& id)
{
	auto it = _staged.find(id);
	if (it == _staged.end())
		return;

	assert_r(removeTempPreviewDirWithRetry(it->tempPreviewDir));
	delete it->item;
	_staged.erase(it);
}

void ImportDialog::updateCardLabelDots(const MediaId& id)
{
	const auto it = _staged.constFind(id);
	if (it == _staged.constEnd())
		return;

	std::vector<QColor> colors;
	QStringList names;
	for (const QString& labelId : it->pendingLabelIds)
	{
		if (const LabelOption* option = findLabelOption(labelId))
		{
			colors.push_back(QColor(option->color));
			names << option->displayName;
		}
	}

	auto* card = static_cast<MediaItemWidget*>(_stagedGrid->itemWidget(it->item));
	card->setLabelDots(colors, names.join(", "));
}

int ImportDialog::stagedPreviewFrameCount() const
{
	return _previewFrameCountCombo->frameCount();
}

void ImportDialog::stagedPreviewFrameCountChanged()
{
	QSettings settings;
	const int mainWindowFrameCount = settings.value(Settings::PreviewFrameCount, Defaults::PreviewFrameCount).toInt();
	const int previousFrameCount = settings.value(STAGED_PREVIEW_FRAME_COUNT_KEY, mainWindowFrameCount).toInt();
	const int currentFrameCount = stagedPreviewFrameCount();
	settings.setValue(STAGED_PREVIEW_FRAME_COUNT_KEY, currentFrameCount);

	if (currentFrameCount > previousFrameCount)
		regenerateInsufficientStagedVideoPreviews(std::max(currentFrameCount, mainWindowFrameCount));
	rebuildAllStagedCards();
}

void ImportDialog::regenerateInsufficientStagedVideoPreviews(int frameCount)
{
	std::vector<Ffmpeg::PreviewJob> jobs;
	std::vector<MediaId> jobIds;
	QStringList cleanupFailures;
	for (auto it = _staged.constBegin(); it != _staged.constEnd(); ++it)
	{
		if (it->tempPreviewDir.isEmpty())
			continue;
		if (listFrameImageFiles(QDir{ it->tempPreviewDir }).size() >= frameCount)
			continue;
		if (!removeTempPreviewDirWithRetry(it->tempPreviewDir))
		{
			cleanupFailures << QDir::toNativeSeparators(it->path);
			continue;
		}
		jobs.push_back({ it->path, it->tempPreviewDir });
		jobIds.push_back(it.key());
	}

	const std::vector<Ffmpeg::PreviewResult> results = generatePreviewFramesInteractively(jobs, frameCount, this);
	for (size_t i = 0; i < results.size(); ++i)
	{
		auto staged = _staged.find(jobIds[i]);
		if (results[i].ok())
			staged->durationMs = results[i].durationMs;
		else
			assert_r(removeTempPreviewDirWithRetry(staged->tempPreviewDir));
	}

	if (!cleanupFailures.isEmpty())
		MessageBox::notice(this, tr("Preview refresh"), tr("Could not clear preview files for:"), cleanupFailures.join("\n"), QMessageBox::Warning);
}

void ImportDialog::zoomStagedCards(int steps)
{
	const int current = stagedCardImageHeight();
	const int next = qBound(MIN_STAGED_CARD_IMAGE_HEIGHT, current + steps * STAGED_CARD_IMAGE_HEIGHT_STEP, MAX_STAGED_CARD_IMAGE_HEIGHT);
	if (next == current)
		return;

	QSettings{}.setValue(STAGED_CARD_IMAGE_HEIGHT_KEY, next);
	_stagedCardZoomDebounce->start();
}

void ImportDialog::rebuildAllStagedCards()
{
	for (auto it = _staged.begin(); it != _staged.end(); ++it)
	{
		MediaItemWidget* card = buildStagedCard(it.key(), it->path, it->tempPreviewDir, it->durationMs);
		card->setInBest(it->pendingBest);
		it->item->setSizeHint(card->sizeHint());
		_stagedGrid->setItemWidget(it->item, card);
		updateCardLabelDots(it.key());
	}
}

std::vector<MediaId> ImportDialog::effectiveStagedSelection(const MediaId& id) const
{
	const QList<QListWidgetItem*> selected = _stagedGrid->selectedItems();
	if (selected.size() <= 1 || !selected.contains(_staged.value(id).item))
		return { id };

	std::vector<MediaId> targets;
	for (auto it = _staged.constBegin(); it != _staged.constEnd(); ++it)
		if (selected.contains(it->item))
			targets.push_back(it.key());
	return targets;
}

void ImportDialog::showStagedCardContextMenu(const MediaId& id, const QPoint& globalPos)
{
	if (!_staged.contains(id))
		return;
	const std::vector<MediaId> selection = effectiveStagedSelection(id);

	QMenu menu(this);

	std::vector<MediaId> photoSelection;
	for (const MediaId& sel : selection)
		if (isSupportedImageFile(_staged.value(sel).path))
			photoSelection.push_back(sel);
	if (photoSelection.size() == selection.size() && photoSelection.size() >= 2)
	{
		menu.addAction(tr("Compare photos"), this, [this, photoSelection] { compareStagedPhotos(photoSelection); });
		menu.addSeparator();
	}

	const bool isPhoto = isSupportedImageFile(_staged.value(id).path);
	menu.addAction(isPhoto ? tr("Open photo") : tr("Play source video"), this, [this, id] { previewStagedItem(id); });
	menu.addAction(tr("Locate source file"), this, [this, id] { locateStagedSourceFile(id); });
	menu.addAction(tr("Copy source path to clipboard"), this, [this, id] { copyStagedSourcePath(id); });
	// Rebuilding the card deletes the widget whose context-menu handler is on the stack.
	QAction* renameItem = menu.addAction(tr("Rename..."), this, [this, id] {
		QMetaObject::invokeMethod(this, [this, id] { renameStagedItem(id); }, Qt::QueuedConnection);
	});
	renameItem->setShortcut(QKeySequence(Shortcuts::Rename));
	renameItem->setShortcutContext(Qt::WidgetShortcut);
	menu.addSeparator();

	const bool allBest = std::all_of(selection.cbegin(), selection.cend(),
		[this](const MediaId& sel) { return _staged.value(sel).pendingBest; });
	menu.addAction(allBest ? tr("Remove from Best") : tr("Add to Best"), this, [this, selection, allBest] {
		setBestForStagedSelection(selection, !allBest);
	});

	std::vector<LabelVisuals::ChecklistRow> labelRows;
	for (const LabelOption& option : _labelOptions)
	{
		int haveCount = 0;
		for (const MediaId& sel : selection)
			if (_staged.value(sel).pendingLabelIds.contains(option.id))
				++haveCount;

		labelRows.push_back({ option.displayName, QColor(option.color),
			LabelVisuals::presenceForCount(haveCount, static_cast<int>(selection.size())),
			[this, selection, labelId = option.id](bool addToAll) {
				for (const MediaId& target : selection)
				{
					auto it = _staged.find(target);
					if (it == _staged.end())
						continue;
					if (addToAll)
					{
						if (!it->pendingLabelIds.contains(labelId))
							it->pendingLabelIds << labelId;
					}
					else
						it->pendingLabelIds.removeAll(labelId);
					updateCardLabelDots(target);
				}
			} });
	}
	QMenu* labelsMenu = menu.addMenu(tr("Labels"));
	QAction* removeAllLabelsAction = labelsMenu->addAction(tr("Remove all labels"), this, [this, selection] {
		for (const MediaId& target : selection)
		{
			auto it = _staged.find(target);
			if (it == _staged.end())
				continue;
			it->pendingLabelIds.clear();
			updateCardLabelDots(target);
		}
	});
	removeAllLabelsAction->setEnabled(std::any_of(selection.cbegin(), selection.cend(),
		[this](const MediaId& target) { return !_staged.value(target).pendingLabelIds.empty(); }));
	labelsMenu->addSeparator();
	LabelVisuals::buildChecklistMenu(labelsMenu, std::move(labelRows));
	menu.addSeparator();

	QAction* removeItem = menu.addAction(tr("Remove from staging"), this, [this, selection] {
		QMetaObject::invokeMethod(this, [this, selection] { removeStagedItems(selection); }, Qt::QueuedConnection);
	});
	removeItem->setShortcut(QKeySequence(Shortcuts::RemoveFromList));
	removeItem->setShortcutContext(Qt::WidgetShortcut);

	QAction* deleteItem = menu.addAction(tr("Delete source file(s)"), this, [this, selection] {
		QMetaObject::invokeMethod(this, [this, selection] { deleteStagedSourceFiles(selection); }, Qt::QueuedConnection);
	});
	deleteItem->setShortcut(QKeySequence(Shortcuts::DeleteFile));
	deleteItem->setShortcutContext(Qt::WidgetShortcut);

	menu.exec(globalPos);
}

std::vector<MediaId> ImportDialog::selectedStagedIds() const
{
	const QList<QListWidgetItem*> selected = _stagedGrid->selectedItems();
	std::vector<MediaId> ids;
	for (auto it = _staged.constBegin(); it != _staged.constEnd(); ++it)
		if (selected.contains(it->item))
			ids.push_back(it.key());
	return ids;
}

void ImportDialog::previewStagedItem(const MediaId& id)
{
	const auto it = _staged.constFind(id);
	if (it == _staged.constEnd())
		return;
	if (isSupportedImageFile(it->path))
		QDesktopServices::openUrl(QUrl::fromLocalFile(it->path));
	else
		VideoPlayerWindow::createPlayerWindow(_library, it->path, this);
}

void ImportDialog::locateStagedSourceFile(const MediaId& id)
{
	const auto it = _staged.constFind(id);
	if (it == _staged.constEnd())
		return;

	if (!revealInFileManager(it->path))
		reportMissingFile(this, it->path);
}

void ImportDialog::copyStagedSourcePath(const MediaId& id)
{
	const auto it = _staged.constFind(id);
	if (it != _staged.constEnd())
		QApplication::clipboard()->setText(QDir::toNativeSeparators(it->path));
}

void ImportDialog::compareStagedPhotos(const std::vector<MediaId>& photoIds)
{
	QStringList paths;
	for (const MediaId& id : photoIds)
		paths << _staged.value(id).path;
	PhotoCompareWindow::showForFiles(_library, paths, this, [this](const QString& removedPath) {
		const QString removedKey = pathComparisonKey(removedPath);
		for (auto it = _staged.constBegin(); it != _staged.constEnd(); ++it)
		{
			if (pathComparisonKey(it->path) == removedKey)
			{
				const MediaId id = it.key();
				unstage(id);
				return;
			}
		}
	});
}

void ImportDialog::setBestForStagedSelection(const std::vector<MediaId>& ids, bool inBest)
{
	for (const MediaId& id : ids)
	{
		auto it = _staged.find(id);
		if (it == _staged.end())
			continue;
		it->pendingBest = inBest;
		static_cast<MediaItemWidget*>(_stagedGrid->itemWidget(it->item))->setInBest(inBest);
	}
}

void ImportDialog::removeStagedItems(const std::vector<MediaId>& ids)
{
	for (const MediaId& id : ids)
		unstage(id);
}

void ImportDialog::deleteStagedSourceFiles(const std::vector<MediaId>& ids)
{
	if (ids.empty())
		return;

	const QString question = ids.size() == 1
		? tr("Move this source file to Trash?\n\n%1").arg(QDir::toNativeSeparators(_staged.value(ids.front()).path))
		: tr("Move %1 source files to Trash?").arg(ids.size());
	if (QMessageBox::warning(this, tr("Delete source file(s)"), question,
			QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
		return;

	for (const MediaId& id : ids)
	{
		const auto it = _staged.constFind(id);
		if (it != _staged.constEnd() && MediaItemManagement::removePathTrashFirstInteractive(it->path, this))
			unstage(id);
	}
}

void ImportDialog::renameStagedItem(const MediaId& id)
{
	const auto it = _staged.constFind(id);
	if (it == _staged.constEnd())
		return;

	const StagedEntry entry = it.value();
	const QFileInfo oldInfo(entry.path);
	const QString oldBase = oldInfo.completeBaseName();
	const QString suffix = oldInfo.suffix();

	bool ok = false;
	const QString newBase = QInputDialog::getText(this, tr("Rename"),
		suffix.isEmpty() ? tr("New name:") : tr("New name (.%1 is kept):").arg(suffix),
		QLineEdit::Normal, oldBase, &ok).trimmed();
	if (!ok || newBase.isEmpty() || newBase == oldBase)
		return;
	if (const QChar bad = invalidFilenameChar(newBase); !bad.isNull())
	{
		QMessageBox::warning(this, tr("Rename"), tr("Name contains an invalid character: '%1'").arg(bad));
		return;
	}

	const QString newName = suffix.isEmpty() ? newBase : QStringLiteral("%1.%2").arg(newBase, suffix);
	const QString newPath = oldInfo.dir().filePath(newName);
	const MediaId newId = MediaId::fromNameAndSize(newName, id.size());

	if (newId != id && _staged.contains(newId))
	{
		QMessageBox::warning(this, tr("Rename"), tr("Another staged item already has that name and size."));
		return;
	}
	if (QFileInfo::exists(newPath) && QFileInfo(newPath) != oldInfo)
	{
		QMessageBox::warning(this, tr("Rename"), tr("A file named \"%1\" already exists in that folder.").arg(newName));
		return;
	}

	if (!QFile::rename(entry.path, newPath))
	{
		QMessageBox::warning(this, tr("Rename"), tr("Could not rename the file (it may be open elsewhere):\n%1").arg(QDir::toNativeSeparators(entry.path)));
		return;
	}

	// The staged key must always equal the current file's MediaId.
	StagedEntry renamed = entry;
	renamed.path = newPath;
	auto* card = buildStagedCard(newId, newPath, renamed.tempPreviewDir, renamed.durationMs);
	_stagedGrid->setItemWidget(renamed.item, card);
	_staged.remove(id);
	_staged.insert(newId, renamed);

	if (renamed.pendingBest)
		card->setInBest(true);
	updateCardLabelDots(newId);
}

void ImportDialog::materializeUsedProvisionalLabels()
{
	// Materialize only provisionals actually used by a staged item.
	QHash<QString, QString> provisionalToReal;
	for (auto it = _staged.constBegin(); it != _staged.constEnd(); ++it)
	{
		for (const QString& labelId : it->pendingLabelIds)
		{
			if (isProvisionalId(labelId) && !provisionalToReal.contains(labelId))
			{
				const LabelOption* option = findLabelOption(labelId);
				const LabelId createdId = option
					? LabelManagement::createLabelOrReport(_library.catalog(), option->displayName, option->color, this)
					: LabelId::None;
				provisionalToReal.insert(labelId, createdId == LabelId::None ? QString{} : toString(createdId));
			}
		}
	}

	if (provisionalToReal.isEmpty())
		return;

	// Remove successful provisionals before remapping so partial retries cannot recreate them.
	_provisionalLabels.erase(std::remove_if(_provisionalLabels.begin(), _provisionalLabels.end(),
		[&](const LabelOption& o) { return !provisionalToReal.value(o.id).isEmpty(); }), _provisionalLabels.end());
	refreshLabelList();

	if (remapStagedLabelIds(provisionalToReal))
		QMessageBox::warning(this, tr("Import"),
			tr("Some labels could not be created (the name may be reserved or invalid); the affected items were left unlabeled and remain staged."));
}

void ImportDialog::importPhotoGroup(const QString& labelId, const std::vector<MediaId>& photoIds, Import::PhotoImportMode mode, ImportOutcome& outcome)
{
	QStringList photoPaths;
	photoPaths.reserve(photoIds.size());
	for (const MediaId& id : photoIds)
		photoPaths << _staged.value(id).path;

	const std::vector<Import::PhotoResult> results =
		ImportExecution::importPhotosInteractive(_library.catalog(), labelIdFromString(labelId), photoPaths, mode, this);
	for (size_t i = 0; i < photoIds.size() && i < results.size(); ++i)
	{
		const MediaId& id = photoIds[i];
		Import::PhotoResult result = results[i];

		// Reference mode can hit an unresolvable name+size clash with an existing item; the escape
		// hatch imports an owned copy instead, whose auto-rename resolves the clash.
		if (result.status == Import::PhotoStatus::IdCollision
			&& QMessageBox::question(this, tr("Already tracked"),
				tr("%1\n\nhas the same name and size as an item already in the library, so it cannot be referenced in place.\n\n"
				   "Import a copy into the library instead (renamed automatically to avoid the clash)?")
				.arg(QDir::toNativeSeparators(_staged.value(id).path)),
				QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes)
		{
			const std::vector<Import::PhotoResult> retried = ImportExecution::importPhotosInteractive(
				_library.catalog(), labelIdFromString(labelId), { _staged.value(id).path }, Import::PhotoImportMode::Copy, this);
			if (!retried.empty())
				result = retried.front();
		}

		if (result.status != Import::PhotoStatus::Success)
			continue;

		const StagedEntry entry = _staged.value(id);
		outcome.succeededIds.push_back(id);
		// An owned copy may be auto-renamed, so apply metadata to the registered id.
		if (entry.pendingBest)
			outcome.bestItems.push_back(result.registeredId);
		if (entry.pendingLabelIds.size() > 1)
			outcome.extraLabelAssignments.push_back(ExtraLabelAssignment{ result.registeredId, entry.pendingLabelIds.mid(1) });
	}
}

void ImportDialog::importVideoGroup(const QString& labelId, const std::vector<MediaId>& videoIds, SourceRelocation::Mode relocateMode, ImportOutcome& outcome)
{
	const LabelOption* label = findLabelOption(labelId);
	if (!label)
		return;
	Catalog& catalog = _library.catalog();
	const QString storageFolder = catalog.storageFolderForLabel(labelIdFromString(labelId));
	if (storageFolder.isEmpty())
	{
		QMessageBox::warning(this, tr("Import"),
			tr("This label does not have a safe storage path:\n%1").arg(label->displayName));
		return;
	}

	QStringList paths;
	QHash<MediaId, QString> stagedPreviewDirs;
	QHash<MediaId, qint64> stagedDurations;
	paths.reserve(videoIds.size());
	stagedPreviewDirs.reserve(videoIds.size());
	stagedDurations.reserve(videoIds.size());
	for (const MediaId& id : videoIds)
	{
		const StagedEntry entry = _staged.value(id);
		paths << entry.path;
		stagedPreviewDirs.insert(id, entry.tempPreviewDir);
		stagedDurations.insert(id, entry.durationMs);
	}

	const SourceRelocation::BatchResult relocated = SourceRelocation::relocateIfNeeded(
		_library, this, paths, relocateMode, _relocateFolderEdit->text());
	ImportExecution::importVideosInteractive(catalog, relocated.toImport, storageFolder, stagedPreviewDirs, stagedDurations, this);

	for (const MediaId& id : videoIds)
	{
		const StagedEntry entry = _staged.value(id);
		if (relocated.keepStaged.contains(entry.path))
			continue;

		if (relocated.skipped.contains(entry.path))
		{
			outcome.skippedIds.push_back(id);
			continue;
		}

		// Follow relocation so a retry never points at a moved-away source.
		if (const QString newPath = relocated.relocatedTo.value(entry.path); !newPath.isEmpty())
			_staged[id].path = newPath;

		if (!isTrackedUnderLabel(catalog, id, labelId))
			continue;

		outcome.succeededIds.push_back(id);

		if (entry.pendingBest)
			outcome.bestItems.push_back(id);
		if (entry.pendingLabelIds.size() > 1)
			outcome.extraLabelAssignments.push_back(ExtraLabelAssignment{ id, entry.pendingLabelIds.mid(1) });
	}
}

void ImportDialog::runImport()
{
	const bool hasLabeledItems = std::any_of(
		_staged.constBegin(), _staged.constEnd(), [](const StagedEntry& entry) { return !entry.pendingLabelIds.empty(); });
	if (!hasLabeledItems)
	{
		QMessageBox::information(this, tr("Import"), tr("No staged item has been labeled yet."));
		return;
	}

	if (_relocateModeCombo->currentIndex() < 0)
	{
		_relocateModeCombo->setFocus(Qt::OtherFocusReason);
		_relocateModeCombo->showPopup();
		return;
	}

	const RelocateMode relocateMode = static_cast<RelocateMode>(_relocateModeCombo->currentData().toInt());

	if (relocateMode != RelocateMode::LeaveInPlace)
	{
		const bool move = relocateMode == RelocateMode::Move;
		const QString text = move
			? tr("Importing with \"Move\" will move each source file out of its current location, deleting the original. Continue?")
			: tr("Importing with \"Copy\" will copy each source file into its destination. Continue?");
		if (MessageBox::question(this, tr("Import"), text, { move ? tr("Move and import") : tr("Copy and import") },
				/*defaultIndex*/ 0, /*cancellable*/ true, QMessageBox::Warning) != 0)
			return;
	}

	Catalog::ChangeBatchScope catalogChanges(_library.catalog());
	materializeUsedProvisionalLabels();

	// Keep the pre-relocation MediaId: Move may delete the path before bookkeeping completes.
	QHash<QString, std::vector<MediaId>> idsByLabelId;
	for (auto it = _staged.constBegin(); it != _staged.constEnd(); ++it)
		if (!it->pendingLabelIds.isEmpty())
			idsByLabelId[it->pendingLabelIds.constFirst()].push_back(it.key());

	if (idsByLabelId.isEmpty())
	{
		QMessageBox::information(this, tr("Import"), tr("No staged item has been labeled yet."));
		return;
	}

	const Import::PhotoImportMode photoMode =
		relocateMode == RelocateMode::Copy ? Import::PhotoImportMode::Copy :
		relocateMode == RelocateMode::Move ? Import::PhotoImportMode::Move :
		                                     Import::PhotoImportMode::Reference;

	ImportOutcome outcome;
	for (auto it = idsByLabelId.constBegin(); it != idsByLabelId.constEnd(); ++it)
	{
		const LabelOption* option = findLabelOption(it.key());
		if (!option)
			continue;

		std::vector<MediaId> videoIds;
		std::vector<MediaId> photoIds;
		for (const MediaId& id : it.value())
			(isSupportedImageFile(_staged.value(id).path) ? photoIds : videoIds).push_back(id);

		if (!photoIds.empty())
			importPhotoGroup(it.key(), photoIds, photoMode, outcome);
		if (!videoIds.empty())
			importVideoGroup(it.key(), videoIds, relocateMode, outcome);
	}

	if (!outcome.bestItems.empty() || !outcome.extraLabelAssignments.empty())
	{
		Catalog& catalog = _library.catalog();
		Catalog::BatchScope batch(catalog);
		for (const MediaId& id : outcome.bestItems)
			if (catalog.containsMediaItem(id))
				catalog.addLabel(id, Catalog::BestLabelId);
		for (const ExtraLabelAssignment& assignment : outcome.extraLabelAssignments)
		{
			if (!catalog.containsMediaItem(assignment.mediaId))
				continue;
			for (const QString& labelId : assignment.labelIds)
				catalog.addLabel(assignment.mediaId, labelIdFromString(labelId));
		}
	}

	for (const MediaId& id : outcome.succeededIds)
		unstage(id);
	for (const MediaId& id : outcome.skippedIds)
		unstage(id);
}
