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
#include "DarkroomSettings.h"
#include "Shortcuts.h"
#include "DarkroomShortcuts.h"
#include "Windows/FrameExtraction.h"
#include "Windows/ImageViewerWindow.h"
#include "Windows/ImportExecution.h"
#include "Windows/LabelManagement.h"
#include "Windows/LibraryPhotoCompare.h"
#include "Windows/LibraryPlayer.h"
#include "Windows/OpenFileRelease.h"
#include "Windows/PathDeletion.h"
#include "Windows/SourceRelocation.h"
#include "Theme/Theme.h"
#include "theme/ctintedsvgiconengine.h"
#include "Utils.h"
#include "UiComponents/MediaItemWidget.h"
#include "Windows/PhotoCompareWindow.h"
#include "Windows/VideoPlayerWindow.h"

#include "assert/advanced_assert.h"
#include "compiler/compiler_warnings_control.h"
#include "dialogs/messagedialog.h"
#include "utils/filemanager.h"
#include "utils/naturalsorting/cnaturalsorterqcollator.h"
#include "widgets/cpersistenceenabler.h"

DISABLE_COMPILER_WARNINGS
#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QColorDialog>
#include <QComboBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QSet>
#include <QSettings>
#include <QSplitter>
#include <QTabWidget>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>
RESTORE_COMPILER_WARNINGS

#include <algorithm>
#include <utility>

namespace {

constexpr int kLabelIdRole = Qt::UserRole;

const QString STAGED_CARD_IMAGE_HEIGHT_KEY = QStringLiteral("importDialog/cardImageHeight");
const QString STAGED_PREVIEW_FRAME_COUNT_KEY = QStringLiteral("importDialog/previewFrameCount");
constexpr int DEFAULT_STAGED_CARD_IMAGE_HEIGHT = 120;
constexpr int MIN_STAGED_CARD_IMAGE_HEIGHT = 60;
constexpr int MAX_STAGED_CARD_IMAGE_HEIGHT = 360;
constexpr int STAGED_CARD_IMAGE_HEIGHT_STEP = 20;

constexpr int LABEL_LIST_MAX_WIDTH = 300;

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

} // namespace

class ImportDialog::StagedGridItem final : public QListWidgetItem
{
public:
	StagedGridItem(StagedId id, QString name, bool sortDescending) : stagedId(id), fileName(std::move(name)), descending(sortDescending) {}

	bool operator<(const QListWidgetItem& other) const override
	{
		const QString& otherName = static_cast<const StagedGridItem&>(other).fileName;
		return descending
			? NaturalSort::lessCaseInsensitive(otherName, fileName)
			: NaturalSort::lessCaseInsensitive(fileName, otherName);
	}

	const StagedId stagedId;
	QString fileName;
	bool descending = false;
};

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

	QPushButton* suggestLabelsButton = new QPushButton(tr("Suggest labels"));
	connect(suggestLabelsButton, &QPushButton::clicked, this, &ImportDialog::suggestLabels);
	labelPaneLayout->addWidget(suggestLabelsButton);

	QPushButton* addLabelButton = new QPushButton(tr("Create label"));
	addLabelButton->setObjectName("addLabelButton");
	addLabelButton->setIcon(tintedSvgIcon(QStringLiteral(":/UI/icon_plus.svg"), [] { return Theme::current().palette.text; }));
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

	_stagedTabs = new QTabWidget();
	_sortDirectionToggle = new SegmentedToggle({ tr("A → Z"), tr("Z → A") });
	connect(_sortDirectionToggle, &SegmentedToggle::currentChanged, this, &ImportDialog::resortStagedItems);
	_stagedTabs->setCornerWidget(_sortDirectionToggle, Qt::TopRightCorner);

	const auto addStagedTab = [this](QHBoxLayout* toolbar, QListWidget* grid, QPushButton* importButton) {
		auto* renameStagedAction = new QAction(tr("Rename..."), this);
		renameStagedAction->setShortcut(QKeySequence(Shortcuts::Rename));
		renameStagedAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
		connect(renameStagedAction, &QAction::triggered, this, [this, grid] {
			const std::vector<StagedId> ids = selectedStagedIds(grid);
			if (ids.size() == 1)
				renameStagedItem(ids.front());
		});
		grid->addAction(renameStagedAction);

		auto* removeStagedAction = new QAction(tr("Remove from staging"), this);
		removeStagedAction->setShortcut(QKeySequence(Shortcuts::RemoveFromList));
		removeStagedAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
		connect(removeStagedAction, &QAction::triggered, this, [this, grid] { removeStagedItems(selectedStagedIds(grid)); });
		grid->addAction(removeStagedAction);

		auto* deleteStagedAction = new QAction(tr("Delete source file(s)"), this);
		deleteStagedAction->setShortcut(QKeySequence(Shortcuts::DeleteFile));
		deleteStagedAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
		connect(deleteStagedAction, &QAction::triggered, this, [this, grid] { deleteStagedSourceFiles(selectedStagedIds(grid)); });
		grid->addAction(deleteStagedAction);

		QHBoxLayout* footer = new QHBoxLayout;
		footer->addStretch(1);
		footer->addWidget(importButton);

		QWidget* page = new QWidget();
		QVBoxLayout* pageLayout = new QVBoxLayout(page);
		pageLayout->addLayout(toolbar);
		pageLayout->addWidget(grid, 1);
		pageLayout->addLayout(footer);
		_stagedTabs->addTab(page, {});
	};

	_videoRelocateModeCombo = new QComboBox();
	_videoRelocateModeCombo->addItem(tr("Leave in place"), int(RelocateMode::LeaveInPlace));
	_videoRelocateModeCombo->addItem(tr("Copy to:"), int(RelocateMode::Copy));
	_videoRelocateModeCombo->addItem(tr("Move to:"), int(RelocateMode::Move));
	_videoRelocateModeCombo->setPlaceholderText(tr("Choose how to handle source videos..."));
	_videoRelocateModeCombo->setCurrentIndex(-1);
	_videoRelocateModeCombo->setToolTip(tr("Previews and extracted frames are always stored in the library; this only affects the source video file."));

	QString relocateFolder = QSettings{}.value("importDialog/relocateFolder").toString();
	if (relocateFolder.isEmpty())
		relocateFolder = suggestedRelocateFolder;
	_relocateFolderEdit = new QLineEdit(relocateFolder);
	QPushButton* relocateBrowseButton = new QPushButton(tr("Browse..."));

	const auto updateRelocateRowEnabled = [this, relocateBrowseButton] {
		const auto modeData = _videoRelocateModeCombo->currentData();
		const bool enabled = modeData.isValid() && modeData.toInt() != int(RelocateMode::LeaveInPlace);
		_relocateFolderEdit->setEnabled(enabled);
		relocateBrowseButton->setEnabled(enabled);
	};
	updateRelocateRowEnabled();
	connect(_videoRelocateModeCombo, &QComboBox::currentIndexChanged, this, updateRelocateRowEnabled);

	connect(relocateBrowseButton, &QPushButton::clicked, this, [this] {
		const QString path = QFileDialog::getExistingDirectory(this, tr("Select destination folder"), _relocateFolderEdit->text());
		if (!path.isEmpty())
			_relocateFolderEdit->setText(QDir::toNativeSeparators(path));
	});

	const int mainWindowFrameCount = QSettings{}.value(Settings::PreviewFrameCount, Defaults::PreviewFrameCount).toInt();
	const int savedStagedFrameCount = QSettings{}.value(STAGED_PREVIEW_FRAME_COUNT_KEY, mainWindowFrameCount).toInt();
	_previewFrameCountCombo = new PreviewFrameCountCombo(savedStagedFrameCount);
	QSettings{}.setValue(STAGED_PREVIEW_FRAME_COUNT_KEY, _previewFrameCountCombo->frameCount());
	connect(_previewFrameCountCombo, &QComboBox::currentIndexChanged, this, &ImportDialog::stagedPreviewFrameCountChanged);

	QHBoxLayout* videoToolbar = new QHBoxLayout;
	videoToolbar->addWidget(new QLabel(tr("Source videos:")));
	videoToolbar->addWidget(_videoRelocateModeCombo);
	videoToolbar->addWidget(_relocateFolderEdit, 1);
	videoToolbar->addWidget(relocateBrowseButton);
	videoToolbar->addWidget(_previewFrameCountCombo, 0, Qt::AlignVCenter);

	QPushButton* importVideosButton = new QPushButton(tr("Import videos"));
	connect(importVideosButton, &QPushButton::clicked, this, &ImportDialog::runVideoImport);
	_videoGrid = new MediaGrid();
	addStagedTab(videoToolbar, _videoGrid, importVideosButton);

	_photoModeCombo = new QComboBox();
	_photoModeCombo->addItem(tr("Reference in place (never touched)"), int(Import::PhotoImportMode::Reference));
	_photoModeCombo->addItem(tr("Copy into the library"), int(Import::PhotoImportMode::Copy));
	_photoModeCombo->addItem(tr("Move into the library"), int(Import::PhotoImportMode::Move));
	_photoModeCombo->setPlaceholderText(tr("Choose how to import photos..."));
	_photoModeCombo->setCurrentIndex(-1);
	_photoModeCombo->setToolTip(tr("A copied or moved photo is stored in the library folder of its first label."));

	QHBoxLayout* photoToolbar = new QHBoxLayout;
	photoToolbar->addWidget(new QLabel(tr("Photos:")));
	photoToolbar->addWidget(_photoModeCombo);
	photoToolbar->addStretch(1);

	QPushButton* importPhotosButton = new QPushButton(tr("Import photos"));
	connect(importPhotosButton, &QPushButton::clicked, this, &ImportDialog::runPhotoImport);
	_photoGrid = new MediaGrid();
	addStagedTab(photoToolbar, _photoGrid, importPhotosButton);

	_stagedCardZoomDebounce = new QTimer(this);
	_stagedCardZoomDebounce->setSingleShot(true);
	_stagedCardZoomDebounce->setInterval(80);
	connect(_stagedCardZoomDebounce, &QTimer::timeout, this, &ImportDialog::rebuildAllStagedCards);

	_splitter->addWidget(_stagedTabs);

	_splitter->setStretchFactor(0, 0);
	_splitter->setStretchFactor(1, 1);
	_splitter->setCollapsible(0, false);

	QLabel* instructions = new QLabel(
		tr("Drop video or image files here to stage them. Assign labels by dragging them from the list onto a card or through the card's right-click menu. "
		   "Dropping a folder stages the media under it; click \"Suggest labels\" to apply labels derived from folder names. Right-click a "
		   "label to rename, recolor, or remove it before importing. "
		   "Double-click a card to preview; right-click for more options. Each tab's import button imports every labeled card "
		   "on that tab and clears it from staging."), this);
	instructions->setWordWrap(true);
	instructions->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::current().palette.textDim.name()));
	outerLayout->addWidget(instructions);

	refreshLabelList();
	updateStagedTabTitles();

	resize(1200, 800);
	setWindowState(Qt::WindowMaximized);
	enablePersistence(this, "importDialog/window", CPersistenceEnabler::Delayed{ true }, CPersistenceEnabler::SetDefaultSize{ false });

	const QByteArray splitterState = QSettings{}.value("importDialog/splitter").toByteArray();
	if (!splitterState.isEmpty())
		_splitter->restoreState(splitterState);
}

ImportDialog::~ImportDialog()
{
	QSettings{}.setValue("importDialog/splitter", _splitter->saveState());
	QSettings{}.setValue("importDialog/relocateFolder", _relocateFolderEdit->text());

	for (const StagedEntry& entry : std::as_const(_staged))
		assert_r(removeTempPreviewDirWithRetry(entry.tempPreviewDir));
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

	_labelList->refreshContentWidth();
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

MediaItemWidget* ImportDialog::buildStagedCard(StagedGridItem* item, const QString& path, const QString& tempPreviewDir, qint64 durationMs)
{
	const int imageHeight = stagedCardImageHeight();
	QSize canvasSize{ imageHeight, imageHeight };
	const bool isPhoto = isSupportedImageFile(path);
	QStringList previewPaths{ path };
	if (!isPhoto)
	{
		const int frameCount = stagedPreviewFrameCount();
		canvasSize.setWidth(MediaItemWidget::videoCanvasWidthForTiling(imageHeight, frameCount, _videoGrid->spacing()));
		const QDir previewDir(tempPreviewDir);
		previewPaths = pickEvenlySpacedFrames(previewDir, listFrameImageFiles(previewDir), frameCount);
	}

	auto* card = new MediaItemWidget(
		canvasSize,
		previewPaths, QFileInfo(path).fileName(),
		/*inBest*/ false,
		[this, item] {
			auto it = _staged.find(item->stagedId);
			if (it != _staged.end())
				it->pendingBest = !it->pendingBest;
		},
		[this, item] { previewStagedItem(item->stagedId); },
		[this, item](QPoint globalPos) { showStagedCardContextMenu(item, globalPos); },
		/* dynamicSizeHint */ false,
		/* film strip */ !isPhoto
	);

	card->setOnLabelDropped([this, item](const QString& labelId) {
		for (const StagedId target : effectiveStagedSelection(item))
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

void ImportDialog::updateStagedTabTitles()
{
	_stagedTabs->setTabText(_stagedTabs->indexOf(_videoGrid->parentWidget()), tr("Videos (%1)").arg(_videoGrid->count()));
	_stagedTabs->setTabText(_stagedTabs->indexOf(_photoGrid->parentWidget()), tr("Photos (%1)").arg(_photoGrid->count()));
}

void ImportDialog::resortStagedItems()
{
	const bool descending = _sortDirectionToggle->currentIndex() == 1;
	for (QListWidget* grid : { _videoGrid, _photoGrid })
	{
		for (int row = 0; row < grid->count(); ++row)
			static_cast<StagedGridItem*>(grid->item(row))->descending = descending;
		grid->sortItems(Qt::AscendingOrder);
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

	// Content already staged is not staged again; it is reported unless the same file was dropped again.
	// Size gates the byte compare.
	QHash<qint64, QStringList> stagedPathsBySize;
	for (const StagedEntry& entry : std::as_const(_staged))
		stagedPathsBySize[QFileInfo(entry.path).size()] << entry.path;

	QStringList newPaths;
	QStringList duplicateLines;
	for (const StagedFile& file : mediaFiles)
	{
		const QString& path = file.path;
		QStringList& sameSizePaths = stagedPathsBySize[QFileInfo(path).size()];
		const auto staged = std::ranges::find_if(sameSizePaths, [&path](const QString& stagedPath) { return filesAreIdentical(path, stagedPath); });
		if (staged == sameSizePaths.end())
		{
			sameSizePaths << path;
			newPaths << path;
		}
		else if (QFileInfo(path) != QFileInfo(*staged))
			duplicateLines << tr("%1\n    is already staged as %2").arg(QDir::toNativeSeparators(path), QDir::toNativeSeparators(*staged));
	}

	QStringList videoPaths;
	QStringList photoPaths;
	const Catalog& catalog = _library.catalog();
	for (const QString& path : newPaths)
	{
		if (const MediaItemId existing = catalog.findMediaItemWithSameContent(path); existing != MediaItemId::None)
			duplicateLines << tr("%1\n    is already imported as %2").arg(QDir::toNativeSeparators(path), QDir::toNativeSeparators(catalog.sourcePathForMediaItem(existing)));
		else
			(isSupportedImageFile(path) ? photoPaths : videoPaths) << path;
	}

	if (!duplicateLines.isEmpty())
		MessageDialog::notice(this, tr("Duplicates"), tr("Not staged - the same content is already staged or in the library:"),
			duplicateLines.join("\n\n"), QMessageBox::Information);

	const int mainWindowFrameCount = QSettings{}.value(Settings::PreviewFrameCount, Defaults::PreviewFrameCount).toInt();
	const int extractionFrameCount = std::max(stagedPreviewFrameCount(), mainWindowFrameCount);

	const auto stageCard = [this, &suggestedLabelNameByPath](QListWidget* grid, const QString& path, const QString& tempPreviewDir, qint64 durationMs = -1) {
		const StagedId id{ ++_lastStagedId };
		auto* item = new StagedGridItem(id, QFileInfo(path).fileName(), _sortDirectionToggle->currentIndex() == 1);
		auto* card = buildStagedCard(item, path, tempPreviewDir, durationMs);
		item->setSizeHint(card->sizeHint());
		grid->addItem(item);
		grid->setItemWidget(item, card);

		_staged.insert(id, StagedEntry{
			path, tempPreviewDir, suggestedLabelNameByPath.value(path), durationMs, /*pendingBest*/ false, /*pendingLabelIds*/ {}, item });
	};

	for (const QString& path : photoPaths)
		stageCard(_photoGrid, path, /*tempPreviewDir*/ {});

	bool anyVideoStaged = false;
	if (!videoPaths.isEmpty())
	{
		std::vector<Ffmpeg::PreviewJob> jobs;
		jobs.reserve(videoPaths.size());
		for (const QString& path : videoPaths)
			jobs.push_back({ path, uniqueTempPreviewDir() });

		const std::vector<Ffmpeg::PreviewResult> results =
			FrameExtraction::generatePreviewFramesInteractively(jobs, extractionFrameCount, tr("Staging"), this);

		for (size_t i = 0; i < jobs.size(); ++i)
		{
			// Cancelled jobs never become staged entries that could own their scratch directory.
			if (results[i].status == Ffmpeg::PreviewResult::Status::Cancelled)
				assert_r(removeTempPreviewDirWithRetry(jobs[i].destinationFolder));
			else
			{
				stageCard(_videoGrid, jobs[i].videoFilePath, jobs[i].destinationFolder, results[i].durationMs);
				anyVideoStaged = true;
			}
		}
	}

	resortStagedItems();
	updateStagedTabTitles();
	const bool anyPhotoStaged = !photoPaths.isEmpty();
	if (anyPhotoStaged != anyVideoStaged)
		_stagedTabs->setCurrentWidget((anyPhotoStaged ? _photoGrid : _videoGrid)->parentWidget());
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

void ImportDialog::unstage(StagedId id)
{
	auto it = _staged.find(id);
	if (it == _staged.end())
		return;

	assert_r(removeTempPreviewDirWithRetry(it->tempPreviewDir));
	delete it->item;
	_staged.erase(it);
	updateStagedTabTitles();
}

void ImportDialog::updateCardLabelDots(StagedId id)
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

	auto* card = static_cast<MediaItemWidget*>(it->item->listWidget()->itemWidget(it->item));
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
	std::vector<StagedId> jobIds;
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

	const std::vector<Ffmpeg::PreviewResult> results =
		FrameExtraction::generatePreviewFramesInteractively(jobs, frameCount, tr("Staging"), this);
	for (size_t i = 0; i < results.size(); ++i)
	{
		auto staged = _staged.find(jobIds[i]);
		if (results[i].ok())
			staged->durationMs = results[i].durationMs;
		else
			assert_r(removeTempPreviewDirWithRetry(staged->tempPreviewDir));
	}

	if (!cleanupFailures.isEmpty())
		MessageDialog::notice(this, tr("Preview refresh"), tr("Could not clear preview files for:"), cleanupFailures.join("\n"), QMessageBox::Warning);
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
		MediaItemWidget* card = buildStagedCard(it->item, it->path, it->tempPreviewDir, it->durationMs);
		card->setInBest(it->pendingBest);
		it->item->setSizeHint(card->sizeHint());
		it->item->listWidget()->setItemWidget(it->item, card);
		updateCardLabelDots(it.key());
	}
}

ImportDialog::StagedId ImportDialog::stagedIdAtPath(const QString& path) const
{
	const QString key = pathComparisonKey(path);
	for (auto it = _staged.constBegin(); it != _staged.constEnd(); ++it)
		if (pathComparisonKey(it->path) == key)
			return it.key();
	return StagedId::None;
}

std::vector<ImportDialog::StagedId> ImportDialog::effectiveStagedSelection(const StagedGridItem* item) const
{
	const QList<QListWidgetItem*> selected = item->listWidget()->selectedItems();
	if (selected.size() <= 1 || !item->isSelected())
		return { item->stagedId };

	std::vector<StagedId> targets;
	targets.reserve(selected.size());
	for (const QListWidgetItem* selectedItem : selected)
		targets.push_back(static_cast<const StagedGridItem*>(selectedItem)->stagedId);
	return targets;
}

void ImportDialog::showStagedCardContextMenu(StagedGridItem* item, const QPoint& globalPos)
{
	if (!_staged.contains(item->stagedId))
		return;
	const std::vector<StagedId> selection = effectiveStagedSelection(item);

	QMenu menu(this);

	const bool isPhoto = item->listWidget() == _photoGrid;
	if (isPhoto && selection.size() >= 2)
	{
		menu.addAction(tr("Compare photos"), this, [this, selection] { compareStagedPhotos(selection); });
		menu.addSeparator();
	}

	menu.addAction(isPhoto ? tr("Open photo") : tr("Play source video"), this, [this, item] { previewStagedItem(item->stagedId); });
	menu.addAction(tr("Locate source file"), this, [this, item] { locateStagedSourceFile(item->stagedId); });
	menu.addAction(tr("Copy source path to clipboard"), this, [this, item] { copyStagedSourcePath(item->stagedId); });
	// Rebuilding the card deletes the widget whose context-menu handler is on the stack.
	QAction* renameItem = menu.addAction(tr("Rename..."), this, [this, item] {
		QMetaObject::invokeMethod(this, [this, item] { renameStagedItem(item->stagedId); }, Qt::QueuedConnection);
	});
	renameItem->setShortcut(QKeySequence(Shortcuts::Rename));
	renameItem->setShortcutContext(Qt::WidgetShortcut);
	menu.addSeparator();

	const bool allBest = std::all_of(selection.cbegin(), selection.cend(),
		[this](StagedId sel) { return _staged.value(sel).pendingBest; });
	menu.addAction(allBest ? tr("Remove from Best") : tr("Add to Best"), this, [this, selection, allBest] {
		setBestForStagedSelection(selection, !allBest);
	});

	std::vector<LabelVisuals::ChecklistRow> labelRows;
	for (const LabelOption& option : _labelOptions)
	{
		int haveCount = 0;
		for (const StagedId sel : selection)
			if (_staged.value(sel).pendingLabelIds.contains(option.id))
				++haveCount;

		labelRows.push_back({ option.displayName, QColor(option.color),
			LabelVisuals::presenceForCount(haveCount, static_cast<int>(selection.size())),
			[this, selection, labelId = option.id](bool addToAll) {
				for (const StagedId target : selection)
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
		for (const StagedId target : selection)
		{
			auto it = _staged.find(target);
			if (it == _staged.end())
				continue;
			it->pendingLabelIds.clear();
			updateCardLabelDots(target);
		}
	});
	removeAllLabelsAction->setEnabled(std::any_of(selection.cbegin(), selection.cend(),
		[this](const StagedId target) { return !_staged.value(target).pendingLabelIds.empty(); }));
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

std::vector<ImportDialog::StagedId> ImportDialog::selectedStagedIds(const QListWidget* grid)
{
	const QList<QListWidgetItem*> selected = grid->selectedItems();
	std::vector<StagedId> ids;
	ids.reserve(selected.size());
	for (const QListWidgetItem* item : selected)
		ids.push_back(static_cast<const StagedGridItem*>(item)->stagedId);
	return ids;
}

void ImportDialog::previewStagedItem(StagedId id)
{
	const auto it = _staged.constFind(id);
	if (it == _staged.constEnd())
		return;

	// Both take the window's current path and return whether the item left the staging list.
	const auto removeStaged = [this](const QString& path) {
		const StagedId stagedId = stagedIdAtPath(path);
		removeStagedItems({ stagedId });
		return !_staged.contains(stagedId);
	};
	const auto deleteStagedSource = [this](const QString& path) {
		const StagedId stagedId = stagedIdAtPath(path);
		deleteStagedSourceFiles({ stagedId });
		return !_staged.contains(stagedId);
	};

	if (!isSupportedImageFile(it->path))
	{
		VideoPlayerWindow* const player = LibraryPlayer::open(_library, it->path, this);
		player->setRemovalActions({ tr("Remove from staging"), removeStaged }, { tr("Delete source file"), deleteStagedSource });
		return;
	}

	QStringList photoPaths;
	int startIndex = -1;
	for (int row = 0; row < _photoGrid->count(); ++row)
	{
		const auto* item = static_cast<const StagedGridItem*>(_photoGrid->item(row));
		if (item->stagedId == id)
			startIndex = static_cast<int>(photoPaths.size());
		photoPaths << _staged.constFind(item->stagedId)->path;
	}

	ImageViewerWindow* const viewer = ImageViewerWindow::showForImages(std::move(photoPaths), startIndex, this,
		[this](const QString& path) { selectAndScrollToStagedItem(stagedIdAtPath(path)); });
	if (!viewer)
		return;

	viewer->setRemovalActions({ tr("Remove from staging"), removeStaged }, { tr("Delete source file"), deleteStagedSource });
}

void ImportDialog::selectAndScrollToStagedItem(StagedId id)
{
	const auto it = _staged.constFind(id);
	if (it == _staged.constEnd())
		return;

	QListWidget* const grid = it->item->listWidget();
	_stagedTabs->setCurrentWidget(grid->parentWidget());
	grid->setCurrentItem(it->item, QItemSelectionModel::ClearAndSelect);
	grid->scrollToItem(it->item);
}

void ImportDialog::locateStagedSourceFile(StagedId id)
{
	const auto it = _staged.constFind(id);
	if (it == _staged.constEnd())
		return;

	if (!revealInFileManager(it->path))
		reportMissingFile(this, it->path);
}

void ImportDialog::copyStagedSourcePath(StagedId id)
{
	const auto it = _staged.constFind(id);
	if (it != _staged.constEnd())
		QApplication::clipboard()->setText(QDir::toNativeSeparators(it->path));
}

void ImportDialog::compareStagedPhotos(const std::vector<StagedId>& photoIds)
{
	QStringList paths;
	for (const StagedId id : photoIds)
		paths << _staged.value(id).path;
	if (PhotoCompareWindow* const compareWindow = PhotoCompareWindow::showForFiles(paths, this))
		LibraryPhotoCompare::installDeleteAction(*compareWindow, _library, [this](const QString& removedPath) { unstage(stagedIdAtPath(removedPath)); });
}

void ImportDialog::setBestForStagedSelection(const std::vector<StagedId>& ids, bool inBest)
{
	for (const StagedId id : ids)
	{
		auto it = _staged.find(id);
		if (it == _staged.end())
			continue;
		it->pendingBest = inBest;
		static_cast<MediaItemWidget*>(it->item->listWidget()->itemWidget(it->item))->setInBest(inBest);
	}
}

void ImportDialog::removeStagedItems(const std::vector<StagedId>& ids)
{
	for (const StagedId id : ids)
		unstage(id);
}

void ImportDialog::deleteStagedSourceFiles(const std::vector<StagedId>& ids)
{
	if (ids.empty())
		return;

	const QString question = ids.size() == 1
		? tr("Move this source file to Trash?\n\n%1").arg(QDir::toNativeSeparators(_staged.value(ids.front()).path))
		: tr("Move %1 source files to Trash?").arg(ids.size());
	if (QMessageBox::warning(this, tr("Delete source file(s)"), question,
			QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
		return;

	QStringList paths;
	for (const StagedId id : ids)
		paths.push_back(_staged.value(id).path);
	const QSet<QString> removedPaths = PathDeletion::removePathsInteractive(paths, PathDeletion::Mode::Trash, this);

	for (const StagedId id : ids)
	{
		if (removedPaths.contains(_staged.value(id).path))
			unstage(id);
	}
}

void ImportDialog::renameStagedItem(StagedId id)
{
	const auto it = _staged.constFind(id);
	if (it == _staged.constEnd())
		return;

	// Copied: the input dialog's event loop may change _staged.
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
	if (QFileInfo::exists(newPath) && QFileInfo(newPath) != oldInfo)
	{
		QMessageBox::warning(this, tr("Rename"), tr("A file named \"%1\" already exists in that folder.").arg(newName));
		return;
	}

	if (!renameReleasingOpenFiles(entry.path, newPath))
	{
		QMessageBox::warning(this, tr("Rename"), tr("Could not rename the file (it may be open elsewhere):\n%1").arg(QDir::toNativeSeparators(entry.path)));
		return;
	}

	const auto renamed = _staged.find(id);
	if (renamed == _staged.end())
		return;
	renamed->path = newPath;
	renamed->item->fileName = newName;
	auto* card = buildStagedCard(renamed->item, newPath, renamed->tempPreviewDir, renamed->durationMs);
	renamed->item->listWidget()->setItemWidget(renamed->item, card);

	if (renamed->pendingBest)
		card->setInBest(true);
	updateCardLabelDots(id);
	resortStagedItems();
}

void ImportDialog::materializeUsedProvisionalLabels(const std::vector<StagedId>& ids)
{
	QHash<QString, QString> provisionalToReal;
	for (const StagedId id : ids)
	{
		const QStringList pendingLabelIds = _staged.value(id).pendingLabelIds;
		for (const QString& labelId : pendingLabelIds)
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

void ImportDialog::importPhotoGroup(const QString& labelId, const std::vector<StagedId>& photoIds, Import::PhotoImportMode mode, ImportOutcome& outcome)
{
	QStringList photoPaths;
	photoPaths.reserve(photoIds.size());
	for (const StagedId id : photoIds)
		photoPaths << _staged.value(id).path;

	const std::vector<Import::PhotoResult> results =
		ImportExecution::importPhotosInteractive(_library.catalog(), labelIdFromString(labelId), photoPaths, mode, this);
	for (size_t i = 0; i < photoIds.size() && i < results.size(); ++i)
	{
		const Import::PhotoResult& result = results[i];
		if (result.status != Import::PhotoStatus::Success)
			continue;

		const StagedEntry entry = _staged.value(photoIds[i]);
		outcome.succeededIds.push_back(photoIds[i]);
		if (entry.pendingBest)
			outcome.bestItems.push_back(result.registeredId);
		if (entry.pendingLabelIds.size() > 1)
			outcome.extraLabelAssignments.push_back(ExtraLabelAssignment{ result.registeredId, entry.pendingLabelIds.mid(1) });
	}
}

void ImportDialog::importVideoGroup(const QString& labelId, const std::vector<StagedId>& videoIds, SourceRelocation::Mode relocateMode, ImportOutcome& outcome)
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
	paths.reserve(videoIds.size());
	for (const StagedId id : videoIds)
		paths << _staged.value(id).path;

	const SourceRelocation::BatchResult relocated = SourceRelocation::relocateIfNeeded(
		_library, this, paths, relocateMode, _relocateFolderEdit->text());

	std::vector<StagedId> importedIds;
	std::vector<ImportExecution::StagedVideo> videos;
	for (const StagedId id : videoIds)
	{
		StagedEntry& entry = _staged[id];
		if (relocated.keepStaged.contains(entry.path))
			continue;

		if (relocated.skipped.contains(entry.path))
		{
			outcome.skippedIds.push_back(id);
			continue;
		}

		// Follow relocation so a retry never points at a moved-away source.
		if (const QString newPath = relocated.relocatedTo.value(entry.path); !newPath.isEmpty())
			entry.path = newPath;

		importedIds.push_back(id);
		videos.push_back({ entry.path, entry.tempPreviewDir, entry.durationMs });
	}

	const std::vector<MediaItemId> registeredIds = ImportExecution::importVideosInteractive(catalog, videos, storageFolder, this);
	for (size_t i = 0; i < importedIds.size(); ++i)
	{
		if (registeredIds[i] == MediaItemId::None)
			continue;

		const StagedEntry entry = _staged.value(importedIds[i]);
		outcome.succeededIds.push_back(importedIds[i]);
		if (entry.pendingBest)
			outcome.bestItems.push_back(registeredIds[i]);
		if (entry.pendingLabelIds.size() > 1)
			outcome.extraLabelAssignments.push_back(ExtraLabelAssignment{ registeredIds[i], entry.pendingLabelIds.mid(1) });
	}
}

std::vector<ImportDialog::StagedId> ImportDialog::labeledIdsReadyForImport(const QListWidget* grid, QComboBox* modeCombo)
{
	std::vector<StagedId> ids;
	for (int row = 0; row < grid->count(); ++row)
	{
		const StagedId id = static_cast<const StagedGridItem*>(grid->item(row))->stagedId;
		if (!_staged.value(id).pendingLabelIds.empty())
			ids.push_back(id);
	}

	if (ids.empty())
	{
		QMessageBox::information(this, tr("Import"),
			grid == _photoGrid ? tr("No staged photo has been labeled yet.") : tr("No staged video has been labeled yet."));
		return {};
	}

	if (modeCombo->currentIndex() < 0)
	{
		modeCombo->setFocus(Qt::OtherFocusReason);
		modeCombo->showPopup();
		return {};
	}

	return ids;
}

void ImportDialog::runPhotoImport()
{
	const std::vector<StagedId> ids = labeledIdsReadyForImport(_photoGrid, _photoModeCombo);
	if (ids.empty())
		return;

	const auto mode = static_cast<Import::PhotoImportMode>(_photoModeCombo->currentData().toInt());
	if (mode != Import::PhotoImportMode::Reference)
	{
		const bool move = mode == Import::PhotoImportMode::Move;
		const QString text = move
			? tr("Importing with \"Move\" will move each photo into the library, deleting the original. Continue?")
			: tr("Importing with \"Copy\" will copy each photo into the library. Continue?");
		if (MessageDialog::question(this, tr("Import"), text, { move ? tr("Move and import") : tr("Copy and import") },
				/*defaultIndex*/ 0, /*cancellable*/ true, QMessageBox::Warning) != 0)
			return;
	}

	importByDestinationLabel(ids, [this, mode](const QString& labelId, const std::vector<StagedId>& photoIds, ImportOutcome& outcome) {
		importPhotoGroup(labelId, photoIds, mode, outcome);
	});
}

void ImportDialog::runVideoImport()
{
	const std::vector<StagedId> ids = labeledIdsReadyForImport(_videoGrid, _videoRelocateModeCombo);
	if (ids.empty())
		return;

	const auto relocateMode = static_cast<RelocateMode>(_videoRelocateModeCombo->currentData().toInt());
	if (relocateMode != RelocateMode::LeaveInPlace)
	{
		const bool move = relocateMode == RelocateMode::Move;
		const QString text = move
			? tr("Importing with \"Move\" will move each source video to the selected folder, deleting the original. Continue?")
			: tr("Importing with \"Copy\" will copy each source video to the selected folder. Continue?");
		if (MessageDialog::question(this, tr("Import"), text, { move ? tr("Move and import") : tr("Copy and import") },
				/*defaultIndex*/ 0, /*cancellable*/ true, QMessageBox::Warning) != 0)
			return;
	}

	importByDestinationLabel(ids, [this, relocateMode](const QString& labelId, const std::vector<StagedId>& videoIds, ImportOutcome& outcome) {
		importVideoGroup(labelId, videoIds, relocateMode, outcome);
	});
}

void ImportDialog::importByDestinationLabel(const std::vector<StagedId>& ids, const GroupImporter& importGroup)
{
	Catalog::ChangeBatchScope catalogChanges(_library.catalog());
	materializeUsedProvisionalLabels(ids);

	QHash<QString, std::vector<StagedId>> idsByLabelId;
	for (const StagedId id : ids)
		if (const QStringList pendingLabelIds = _staged.value(id).pendingLabelIds; !pendingLabelIds.isEmpty())
			idsByLabelId[pendingLabelIds.constFirst()].push_back(id);

	if (idsByLabelId.isEmpty())
	{
		QMessageBox::information(this, tr("Import"), tr("No staged item has been labeled yet."));
		return;
	}

	ImportOutcome outcome;
	for (auto it = idsByLabelId.constBegin(); it != idsByLabelId.constEnd(); ++it)
		if (findLabelOption(it.key()))
			importGroup(it.key(), it.value(), outcome);

	if (!outcome.bestItems.empty() || !outcome.extraLabelAssignments.empty())
	{
		Catalog& catalog = _library.catalog();
		Catalog::BatchScope batch(catalog);
		for (const MediaItemId id : outcome.bestItems)
			if (catalog.containsMediaItem(id))
				catalog.addLabel(id, Catalog::BestLabelId);
		for (const ExtraLabelAssignment& assignment : outcome.extraLabelAssignments)
		{
			if (!catalog.containsMediaItem(assignment.mediaItemId))
				continue;
			for (const QString& labelId : assignment.labelIds)
				catalog.addLabel(assignment.mediaItemId, labelIdFromString(labelId));
		}
	}

	for (const StagedId id : outcome.succeededIds)
		unstage(id);
	for (const StagedId id : outcome.skippedIds)
		unstage(id);
}
