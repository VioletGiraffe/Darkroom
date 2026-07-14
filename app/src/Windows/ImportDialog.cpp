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
#include "Settings.h"
#include "Shortcuts.h"
#include "Windows/SourceRelocation.h"
#include "Theme/Icons.h"
#include "Theme/Theme.h"
#include "Utils.h"
#include "UiComponents/MediaItemWidget.h"
#include "Windows/PhotoCompareWindow.h"
#include "Windows/VideoPlayerWindow.h"

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
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
#include <QThread>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>

namespace {

constexpr int kLabelIdRole = Qt::UserRole;

// Staged cards render at a fixed size - this dialog has no zoom control like the main grid's Ctrl+wheel.
constexpr int STAGED_CARD_IMAGE_HEIGHT = 120;

// The label list hugs its content width (ContentWidthListWidget) but is capped here so a pathologically long
// label name can't grow the pane without bound - past this the row elides instead.
constexpr int LABEL_LIST_MAX_WIDTH = 300;

// A fresh, unique scratch directory for one staged video's temp preview frames.
QString uniqueTempPreviewDir()
{
	return QDir::tempPath() + "/darkroom_import/" + QUuid::createUuid().toString(QUuid::Id128);
}

// One staged file plus the label its origin folder implies. labelName is the path from the dropped folder's
// *parent* down to the file's own folder, components joined by '-' (so a file at ".../Root/cars/2026/x.jpg"
// dropped as the folder "Root" yields "Root-cars-2026"); it's empty for a file dropped directly (not inside a
// dropped folder), which then stages unlabeled.
struct StagedFile
{
	QString path;
	QString labelName;
};

// Expands a dropped path list to the media files to stage, each paired with its folder-derived label name (see
// StagedFile): a supported video/photo file passes through unlabeled; a directory is scanned recursively and each
// supported file under it carries the "-"-joined relative path of its containing folder; anything else
// (unsupported file, dead path) is dropped. Folder handling lives here so it's uniform across every staging entry
// point - the dialog's own drop, addToStaging (the main window's drop), and the untracked scan.
QList<StagedFile> flattenToSupportedMediaFiles(const QStringList& paths)
{
	QList<StagedFile> files;
	for (const QString& path : paths)
	{
		if (QFileInfo(path).isDir())
		{
			// Names are relative to the dropped folder's parent, so the dropped folder's own name leads the label
			// (dropping "Root" -> labels start with "Root", not with whatever sits below it). cleanPath strips a
			// trailing slash, which would otherwise make absolutePath() return the folder itself, not its parent.
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

// True iff the item is tracked by the Catalog at the exact frame folder this import derives for it
// (Import::importVideo's outputFolder). Checked right after a video batch import to tell a successfully-added
// entry (clear from staging) from a declined/failed one (leave staged). Deliberately not "tracked under *some*
// folder": on a name+size collision the id is already tracked elsewhere and the staged copy was refused - a
// plain "is tracked" check would misreport that as imported, silently dropping the entry's pending labels.
[[nodiscard]] bool isTrackedUnderLabel(const Catalog& catalog, const MediaId& id, const QString& labelId)
{
	const QString storageFolder = catalog.storageFolderForLabel(labelIdFromString(labelId));
	if (storageFolder.isEmpty())
		return false;
	const QString expectedFolder = storageFolder + "/" + QFileInfo(id.name()).completeBaseName();
	return QString::compare(catalog.folderForMediaItem(id), expectedFolder, Qt::CaseInsensitive) == 0;
}

} // namespace

ImportDialog::ImportDialog(Library& library, Callbacks callbacks, const QString& suggestedRelocateFolder, QWidget* parent)
	: QDialog(parent)
	, m_library(library)
	, m_callbacks(std::move(callbacks))
{
	setWindowTitle(tr("Import"));
	// Opens maximized and acts as a full workspace window: give it real min/max caption buttons (a QDialog has
	// none by default) and no "?" context-help button.
	setWindowFlags((windowFlags() | Qt::WindowMinMaxButtonsHint) & ~Qt::WindowContextHelpButtonHint);
	setAcceptDrops(true);  // media files dropped anywhere on the dialog are staged - see dragEnterEvent/dropEvent

	QVBoxLayout* outerLayout = new QVBoxLayout(this);

	// --- Source file relocation row ---
	QSettings relocateSettings;
	m_relocateModeCombo = new QComboBox(this);
	// The interim wording notes the photo meaning (Reference) in place; a deeper redesign of the whole
	// owned/referenced model is a flagged post-v1 direction, so this stays a plain static label until then.
	m_relocateModeCombo->addItem(tr("Leave source file in place (photos: reference, never touched)"), int(RelocateMode::LeaveInPlace));
	m_relocateModeCombo->addItem(tr("Copy source file to:"), int(RelocateMode::Copy));
	m_relocateModeCombo->addItem(tr("Move source file to:"), int(RelocateMode::Move));
	const int savedRelocateMode = relocateSettings.value("importDialog/relocateMode", int(RelocateMode::LeaveInPlace)).toInt();
	m_relocateModeCombo->setCurrentIndex(qMax(0, m_relocateModeCombo->findData(savedRelocateMode)));

	// Persisted choice wins; the caller's suggestion only seeds the field on first use.
	QString relocateFolder = relocateSettings.value("importDialog/relocateFolder").toString();
	if (relocateFolder.isEmpty())
		relocateFolder = suggestedRelocateFolder;
	m_relocateFolderEdit = new QLineEdit(relocateFolder, this);
	QPushButton* relocateBrowseButton = new QPushButton(tr("Browse..."), this);

	const auto updateRelocateRowEnabled = [this, relocateBrowseButton] {
		const bool enabled = m_relocateModeCombo->currentData().toInt() != int(RelocateMode::LeaveInPlace);
		m_relocateFolderEdit->setEnabled(enabled);
		relocateBrowseButton->setEnabled(enabled);
	};
	updateRelocateRowEnabled();
	connect(m_relocateModeCombo, &QComboBox::currentIndexChanged, this, updateRelocateRowEnabled);

	connect(relocateBrowseButton, &QPushButton::clicked, this, [this] {
		const QString path = QFileDialog::getExistingDirectory(this, tr("Select destination folder"), m_relocateFolderEdit->text());
		if (!path.isEmpty())
			m_relocateFolderEdit->setText(QDir::toNativeSeparators(path));
	});

	QHBoxLayout* relocateRow = new QHBoxLayout;
	relocateRow->addWidget(new QLabel(tr("Source file:"), this));
	relocateRow->addWidget(m_relocateModeCombo);
	relocateRow->addWidget(m_relocateFolderEdit, 1);
	relocateRow->addWidget(relocateBrowseButton);
	outerLayout->addLayout(relocateRow);

	// --- Main area: [label list | staged video grid], mirroring MainWindow's [LabelSidebar | grid] split ---
	m_splitter = new QSplitter(Qt::Horizontal, this);
	outerLayout->addWidget(m_splitter, 1);

	QWidget* labelPane = new QWidget();
	QVBoxLayout* labelPaneLayout = new QVBoxLayout(labelPane);
	labelPaneLayout->setContentsMargins(0, 0, 0, 0);
	labelPaneLayout->addWidget(new QLabel(tr("Labels")));

	m_labelList = new ContentWidthListWidget();  // hugs its content so the pane auto-fits the label names
	m_labelList->setSelectionMode(QAbstractItemView::NoSelection);  // rows are dragged, never selected
	m_labelList->setFrameShape(QFrame::NoFrame);
	// Cap the content-hugging width so a pathologically long label name can't blow the pane up; the row elides
	// instead (horizontal scrolling off). No explicit minimum width, so the minimum tracks content up to the cap.
	m_labelList->setMaximumWidth(LABEL_LIST_MAX_WIDTH);
	m_labelList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	// LabelRowDelegate paints the rows just like the main window's sidebar: squircle swatch, name, dashed
	// hover outline (mouse tracking on so hover repaints). The active pill/spine states simply never engage
	// here - no row is ever marked active.
	m_labelList->setItemDelegate(new LabelRowDelegate(m_labelList));
	m_labelList->setMouseTracking(true);
	// A press-and-drag on a label row drags the label out, to be dropped onto a staged card.
	new ListRowDragFilter(m_labelList, [](const QListWidgetItem* item) {
		auto* mime = new QMimeData();
		mime->setData(LabelMimeType, item->data(kLabelIdRole).toString().toUtf8());
		return mime;
	});
	m_labelList->setContextMenuPolicy(Qt::CustomContextMenu);  // right-click a row to edit a provisional label
	connect(m_labelList, &QListWidget::customContextMenuRequested, this, &ImportDialog::showLabelListContextMenu);
	labelPaneLayout->addWidget(m_labelList, 1);

	QPushButton* addLabelButton = new QPushButton(tr("Create label"));
	addLabelButton->setObjectName("addLabelButton");
	addLabelButton->setIcon(Theme::tintedIcon(QStringLiteral(":/UI/icon_plus.svg"), &Theme::ThemeColors::TextPrimary));
	// Ctrl+L mirrors LabelSidebar's Create-label button; keep the two in sync. The tooltip surfaces the shortcut
	// (derived from it, so there's a single source of truth) since a button doesn't advertise one otherwise.
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
		// A manually added label is provisional too (created in the Catalog only on Import). Reusing silently on a
		// name clash would surprise here (the user just typed it), so inform instead.
		if (!findLabelIdByName(name, {}).isEmpty())
		{
			QMessageBox::information(this, tr("New Label"), tr("A label named \"%1\" already exists.").arg(name));
			return;
		}
		addProvisionalLabel(name);
		refreshLabelList();
	});
	labelPaneLayout->addWidget(addLabelButton);

	m_splitter->addWidget(labelPane);

	m_stagedGrid = new QListWidget();
	m_stagedGrid->setViewMode(QListView::IconMode);
	m_stagedGrid->setFlow(QListView::LeftToRight);
	m_stagedGrid->setWrapping(true);
	m_stagedGrid->setResizeMode(QListView::Adjust);
	m_stagedGrid->setMovement(QListView::Static);
	m_stagedGrid->setSelectionMode(QAbstractItemView::ExtendedSelection);
	m_stagedGrid->setSpacing(10);
	// The cards are transparent (see MediaItemWidget), so the selection highlight has to come from the list
	// item behind them - same rule the main grid uses.
	m_stagedGrid->setStyleSheet(QStringLiteral("QListWidget::item:selected { background-color: %1; }").arg(Theme::current().AccentBg));

	// Keyboard accelerators on the staged grid, mirroring MainWindow's edit actions: F2 renames, Del removes from
	// staging, Shift+Del deletes the source file(s). WidgetWithChildrenShortcut scopes them to the grid so they don't
	// fire while typing elsewhere in the dialog; the context menu shows the same shortcuts (see showStagedCardContextMenu).
	auto* renameStagedAction = new QAction(tr("Rename..."), this);
	renameStagedAction->setShortcut(QKeySequence(Shortcuts::Rename));
	renameStagedAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
	connect(renameStagedAction, &QAction::triggered, this, [this] {
		const std::vector<MediaId> ids = selectedStagedIds();
		if (ids.size() == 1)
			renameStagedItem(ids.front());   // F2 renames only when exactly one item is selected
	});
	m_stagedGrid->addAction(renameStagedAction);

	auto* removeStagedAction = new QAction(tr("Remove from staging"), this);
	removeStagedAction->setShortcut(QKeySequence(Shortcuts::RemoveFromList));
	removeStagedAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
	connect(removeStagedAction, &QAction::triggered, this, [this] { removeStagedItems(selectedStagedIds()); });
	m_stagedGrid->addAction(removeStagedAction);

	auto* deleteStagedAction = new QAction(tr("Delete source file(s)"), this);
	deleteStagedAction->setShortcut(QKeySequence(Shortcuts::DeleteFile));
	deleteStagedAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
	connect(deleteStagedAction, &QAction::triggered, this, [this] { deleteStagedSourceFiles(selectedStagedIds()); });
	m_stagedGrid->addAction(deleteStagedAction);

	m_splitter->addWidget(m_stagedGrid);

	m_splitter->setStretchFactor(0, 0);
	m_splitter->setStretchFactor(1, 1);
	m_splitter->setCollapsible(0, false);

	QLabel* instructions = new QLabel(
		tr("Drop video or image files here to stage them, then drag labels from the list onto a card to tag it. "
		   "Dropping a folder stages the media under it and makes a label from the folder's name; right-click a "
		   "label to rename, recolor, or remove it before importing. "
		   "Double-click a card to preview; right-click for more options. \"Import\" imports every labeled card "
		   "and clears it from staging."), this);
	instructions->setWordWrap(true);
	instructions->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::current().MutedText));
	outerLayout->addWidget(instructions);

	QHBoxLayout* footer = new QHBoxLayout;
	footer->addStretch(1);
	QPushButton* importButton = new QPushButton(tr("Import"), this);
	connect(importButton, &QPushButton::clicked, this, &ImportDialog::runImport);
	footer->addWidget(importButton);
	outerLayout->addLayout(footer);

	refreshLabelList();

	// Restore persisted window geometry; with nothing stored yet (first run) open maximized - this is a full
	// workspace - letting the saved geometry rule on every run thereafter.
	if (!restoreWindowGeometry(this, "importDialog"))
	{
		resize(1200, 800);  // the size the dialog falls back to when un-maximized
		setWindowState(Qt::WindowMaximized);
	}
	const QByteArray splitterState = QSettings{}.value("importDialog/splitter").toByteArray();
	if (!splitterState.isEmpty())
		m_splitter->restoreState(splitterState);
}

ImportDialog::~ImportDialog()
{
	saveWindowGeometry(this, "importDialog");
	QSettings{}.setValue("importDialog/splitter", m_splitter->saveState());
	QSettings{}.setValue("importDialog/relocateMode", m_relocateModeCombo->currentData().toInt());
	QSettings{}.setValue("importDialog/relocateFolder", m_relocateFolderEdit->text());

	// Best-effort: clean up whatever's still staged when the dialog closes (anything already unstaged or
	// successfully added already removed its own temp dir). The isEmpty guard is load-bearing: staged photos
	// have no temp dir, and QDir("") refers to the working directory - removeRecursively on it would be a disaster.
	for (const StagedEntry& entry : std::as_const(m_staged))
		if (!entry.tempPreviewDir.isEmpty())
			QDir(entry.tempPreviewDir).removeRecursively();
}

void ImportDialog::dragEnterEvent(QDragEnterEvent* event)
{
	if (hasSupportedPaths(event->mimeData()))
		event->acceptProposedAction();
}

void ImportDialog::dropEvent(QDropEvent* event)
{
	// Files and folders both accepted; stageMediaItems expands any folder into the supported files under it.
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
	// The list is this session's provisional labels (folder-derived or manually added) followed by the Catalog's
	// real ones. Provisionals lead so they're easy to review/rename before Import materializes them.
	m_labelOptions.clear();
	for (const LabelOption& provisional : m_provisionalLabels)
		m_labelOptions.push_back(provisional);
	// Every ordinary Catalog label is a candidate destination - read from the Catalog model (which also
	// carries each label's color), not from a disk listing.
	for (const Catalog::Label& label : m_library.catalog().allLabels())
		if (!label.isVirtual())
			m_labelOptions.push_back(LabelOption{ toString(label.id), label.displayName, label.color });

	m_labelList->clear();
	for (const LabelOption& option : m_labelOptions)
	{
		auto* item = new QListWidgetItem(
			option.provisional ? tr("%1  (new)").arg(option.displayName) : option.displayName, m_labelList);
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

	// The row set changed the content width; let the layout/splitter re-fit the pane (mirrors LabelSidebar::rebuildRows).
	m_labelList->updateGeometry();
}

const ImportDialog::LabelOption* ImportDialog::findLabelOption(const QString& id) const
{
	for (const LabelOption& option : m_labelOptions)
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
	// Case-insensitive to match the (Windows) filesystem and Catalog's own label-name uniqueness rule.
	for (const LabelOption& option : m_labelOptions)
		if (option.id != excludeId && option.displayName.compare(name, Qt::CaseInsensitive) == 0)
			return option.id;
	return {};
}

QString ImportDialog::addProvisionalLabel(const QString& name)
{
	LabelOption option;
	option.id = QStringLiteral("new:%1").arg(m_provisionalSeq++);
	option.displayName = name;
	option.color = Catalog::randomLabelColor();  // the swatch it will keep when Import creates it for real
	option.provisional = true;
	m_provisionalLabels.push_back(option);
	m_labelOptions.push_back(option);  // keep the cached union live so a follow-up findLabelId/updateCardLabelDots sees it at once
	return option.id;
}

QString ImportDialog::ensureLabelForFolderName(const QString& name)
{
	if (name.isEmpty())
		return {};
	// A folder whose name already matches a label (real, or an earlier provisional from this same drop) reuses it -
	// "existing labels need no action", and two files from one folder share a single label.
	const QString existing = findLabelIdByName(name, {});
	return existing.isEmpty() ? addProvisionalLabel(name) : existing;
}

void ImportDialog::showLabelListContextMenu(const QPoint& pos)
{
	QListWidgetItem* item = m_labelList->itemAt(pos);
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
		// A real (already-catalogued) label isn't editable from here - explain rather than hide the command.
		menu.addAction(tr("Rename..."), this, [this] {
			QMessageBox::information(this, tr("Rename label"),
				tr("This label already exists in the catalog, you can rename it in the main window. All changes are "
				   "temporary until \"Import\" is clicked. You can create a new label with your desired name; or "
				   "assign the existing label and rename it after import runs."));
		});
	}
	menu.exec(m_labelList->viewport()->mapToGlobal(pos));
}

void ImportDialog::renameProvisionalLabel(const QString& provisionalId)
{
	const auto option = std::find_if(m_provisionalLabels.begin(), m_provisionalLabels.end(),
		[&](const LabelOption& o) { return o.id == provisionalId; });
	if (option == m_provisionalLabels.end())
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

	// Renaming onto an existing label (real or another provisional) offers to merge into it instead of minting a clash.
	if (const QString clashId = findLabelIdByName(newName, provisionalId); !clashId.isEmpty())
	{
		if (QMessageBox::question(this, tr("Merge labels"), tr("A label named \"%1\" already exists. Merge into it?").arg(newName)) == QMessageBox::Yes)
			mergeProvisionalInto(provisionalId, clashId);
		return;  // "No" abandons the rename; the label keeps its current name
	}

	option->displayName = newName;
	refreshLabelList();
	updateAllCardLabelDots();  // a card's dot tooltip is built from label names
}

void ImportDialog::setProvisionalLabelColor(const QString& provisionalId)
{
	const auto option = std::find_if(m_provisionalLabels.begin(), m_provisionalLabels.end(),
		[&](const LabelOption& o) { return o.id == provisionalId; });
	if (option == m_provisionalLabels.end())
		return;

	const QColor initial = option->color.isEmpty() ? QColor(Qt::white) : QColor(option->color);
	const QColor chosen = QColorDialog::getColor(initial, this, tr("Label color"));
	if (!chosen.isValid())
		return;  // dialog cancelled

	option->color = chosen.name();  // "#rrggbb"
	refreshLabelList();
	updateAllCardLabelDots();
}

bool ImportDialog::remapStagedLabelIds(const QHash<QString, QString>& mapping)
{
	bool anyDropped = false;
	for (auto it = m_staged.begin(); it != m_staged.end(); ++it)
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
	// Strip the label from every staged card that carried it, then drop it from the list.
	remapStagedLabelIds({ { provisionalId, QString{} } });

	m_provisionalLabels.erase(std::remove_if(m_provisionalLabels.begin(), m_provisionalLabels.end(),
		[&](const LabelOption& o) { return o.id == provisionalId; }), m_provisionalLabels.end());
	refreshLabelList();
}

void ImportDialog::mergeProvisionalInto(const QString& provisionalId, const QString& targetId)
{
	// Point every staged pick at the target and drop the now-merged provisional. Pre-import this is purely local -
	// nothing in the Catalog changes; it's a rewrite of the staged cards' pending assignments.
	remapStagedLabelIds({ { provisionalId, targetId } });

	m_provisionalLabels.erase(std::remove_if(m_provisionalLabels.begin(), m_provisionalLabels.end(),
		[&](const LabelOption& o) { return o.id == provisionalId; }), m_provisionalLabels.end());
	refreshLabelList();
}

void ImportDialog::updateAllCardLabelDots()
{
	for (auto it = m_staged.constBegin(); it != m_staged.constEnd(); ++it)
		updateCardLabelDots(it.key());
}

void ImportDialog::addToStaging(const QStringList& paths)
{
	// Deferred so the dialog can paint before stageMediaItems() blocks on ffmpeg preview extraction.
	QMetaObject::invokeMethod(this, [this, paths] { stageMediaItems(paths); }, Qt::QueuedConnection);
}

MediaItemWidget* ImportDialog::buildStagedCard(const MediaId& id, const QString& path, const QString& tempPreviewDir, qint64 durationMs)
{
	// Card thumbnail canvas mirrors the main library grid: a photo is square and decodes its file directly; a video
	// is a horizontal strip sized to tile with `frameCount` photo cards (so mixed cards line up on one column grid),
	// showing the frames already extracted into its temp preview dir.
	QSize canvasSize{ STAGED_CARD_IMAGE_HEIGHT, STAGED_CARD_IMAGE_HEIGHT };
	QStringList previewPaths{ path };
	if (!isSupportedImageFile(path))
	{
		const int frameCount = QSettings{}.value(Settings::PreviewFrameCount, Defaults::PreviewFrameCount).toInt();
		canvasSize.setWidth(MediaItemWidget::videoCanvasWidthForTiling(STAGED_CARD_IMAGE_HEIGHT, frameCount, m_stagedGrid->spacing()));
		previewPaths.clear();
		const QDir previewDir(tempPreviewDir);
		for (const QString& file : previewDir.entryList(IMAGE_FILE_FILTERS, QDir::Files, QDir::Name))
			previewPaths << previewDir.filePath(file);
	}

	auto* card = new MediaItemWidget(
		canvasSize,
		previewPaths, QFileInfo(path).fileName(),
		id,
		/*inBest*/ false,
		[this, id] {
			auto it = m_staged.find(id);
			if (it != m_staged.end())
				it->pendingBest = !it->pendingBest;  // the star button's own checked state is Qt's, not ours
		},
		[this, id] { previewStagedItem(id); },   // double-click: open the photo / play the video (same as the menu action)
		[this, id](QPoint globalPos) { showStagedCardContextMenu(id, globalPos); },
		/* dynamicSizeHint */ false,
		/* film strip */ !isSupportedImageFile(path)   // videos read as a perforated film strip; photos stay a plain square
	);

	// A label dropped here tags just this card, or the whole selection if this card is part of one.
	card->setOnLabelDropped([this, id](const QString& labelId) {
		for (const MediaId& target : effectiveStagedSelection(id))
		{
			auto it = m_staged.find(target);
			if (it != m_staged.end() && !it->pendingLabelIds.contains(labelId))
				it->pendingLabelIds << labelId;
			updateCardLabelDots(target);
		}
	});

	// A video with a known length shows the duration pill; a photo (or a not-yet-probed video) passes -1 -> no pill.
	card->setDuration(durationMs);

	return card;
}

void ImportDialog::stageMediaItems(const QStringList& paths)
{
	// A dropped folder contributes every supported file under it (recursive, flattened), each tagged with the
	// label its origin folder implies; plain files pass through unlabeled. Doing it here means folder-drop and its
	// auto-labeling work from every entry point that reaches staging.
	const QList<StagedFile> mediaFiles = flattenToSupportedMediaFiles(paths);
	if (mediaFiles.isEmpty())
		return;

	QHash<QString, QString> labelNameByPath;  // path -> folder-derived label name (only for files that carry one)
	for (const StagedFile& file : mediaFiles)
		if (!file.labelName.isEmpty())
			labelNameByPath.insert(file.path, file.labelName);

	// Dedup by MediaId, both against already-staged entries and within this batch (m_staged is keyed by id,
	// so accepting a second same-id file would silently overwrite the first entry, orphaning its card and
	// leaking its temp dir). The id match (name+size) is the cheap gate; a byte comparison then classifies
	// it, mirroring SourceRelocation's gate-then-verify shape: identical content is a plain duplicate,
	// skipped silently, while different content is a genuine collision the user must know about - the
	// catalog tracks at most one item per id, so the file can't be staged alongside its twin.
	QHash<MediaId, QString> stagedPathById;
	for (auto it = m_staged.constBegin(); it != m_staged.constEnd(); ++it)
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
		QMessageBox::warning(this, tr("Name collision"),
			tr("Not staged - same name and size as an already staged file, but different content. "
			   "Only one item per name+size can be tracked; rename the file if both are wanted.\n\n%1")
			.arg(collisionLines.join("\n\n")));

	if (newPaths.isEmpty())
		return;

	// Photos split off from videos here: they need no ffmpeg preview (the card decodes the file directly),
	// but each is first checked against the already-imported photos for byte-identical content (matched by
	// size, any name - catches renamed duplicates), so a dup is flagged before it can be labeled at all.
	QStringList videoPaths;
	QStringList photoPaths;
	QStringList duplicateLines;
	for (const QString& path : newPaths)
	{
		if (!isSupportedImageFile(path))
		{
			videoPaths << path;
			continue;
		}
		const QString existingPath = m_library.catalog().findPhotoBySameContent(path);
		if (!existingPath.isEmpty())
			duplicateLines << tr("%1\n    is already imported as %2").arg(QDir::toNativeSeparators(path), QDir::toNativeSeparators(existingPath));
		else
			photoPaths << path;
	}

	if (!duplicateLines.isEmpty())
		QMessageBox::information(this, tr("Already imported"),
			tr("Not staged - a photo with identical content is already in the library:\n\n%1").arg(duplicateLines.join("\n\n")));

	// Resolve each staged file's folder label to a concrete id (reusing an existing label or minting a provisional
	// one) and surface the new provisional labels, before the slower video preview extraction below. Only files
	// that survived every filter above are considered, so a drop consisting entirely of duplicates mints no label.
	QHash<QString, QString> labelIdByPath;
	for (const QString& path : videoPaths + photoPaths)
	{
		if (const QString name = labelNameByPath.value(path); !name.isEmpty())
			labelIdByPath.insert(path, ensureLabelForFolderName(name));
	}

	if (!labelIdByPath.isEmpty())
		refreshLabelList();

	const int frameCount = QSettings{}.value(Settings::PreviewFrameCount, Defaults::PreviewFrameCount).toInt();

	// Builds one staged card + entry and inserts it into the grid; the per-type differences (canvas size, preview
	// images) live in buildStagedCard. labelIdByPath is captured by reference - stageCard is only ever called
	// synchronously from within this function.
	const auto stageCard = [this, &labelIdByPath](const QString& path, const QString& tempPreviewDir, qint64 durationMs = -1) {
		const MediaId id = MediaId::fromFile(path);
		auto* card = buildStagedCard(id, path, tempPreviewDir, durationMs);

		auto* item = new QListWidgetItem();
		item->setSizeHint(card->sizeHint());
		m_stagedGrid->addItem(item);
		m_stagedGrid->setItemWidget(item, card);

		m_staged.insert(id, StagedEntry{ path, tempPreviewDir, durationMs, /*pendingBest*/ false, /*pendingLabelIds*/ {}, item });

		// Pre-assign the folder-derived label (the first/destination label), if this file came from a dropped folder.
		if (const QString labelId = labelIdByPath.value(path); !labelId.isEmpty())
		{
			m_staged[id].pendingLabelIds << labelId;
			updateCardLabelDots(id);
		}
	};

	// Photo cards decode the file itself - no extraction step, so they stage instantly.
	for (const QString& path : photoPaths)
		stageCard(path, /*tempPreviewDir*/ {});

	if (videoPaths.isEmpty())
		return;

	// One temp scratch dir per video, assembled up front so the batch can extract several videos at once and
	// the card-building pass below can locate each video's frames.
	std::vector<Ffmpeg::PreviewJob> jobs;
	jobs.reserve(videoPaths.size());
	for (const QString& path : videoPaths)
		jobs.push_back({ path, uniqueTempPreviewDir() });

	QMessageBox progressBox(this);
	progressBox.setWindowTitle(tr("Staging"));
	progressBox.setStandardButtons(QMessageBox::NoButton);
	progressBox.setModal(true);
	progressBox.setText(tr("Generating preview %1/%2...").arg(0).arg(jobs.size()));
	progressBox.show();
	QApplication::processEvents();  // paint the dialog before the batch blocks this thread

	// Extract previews for several videos concurrently - each ffmpeg is its own OS process. Half the cores
	// leaves headroom for ffmpeg's own internal threading and for the UI. Each result carries the duration the
	// probe read, stashed on the staged entry so import records it without probing the same file again.
	const std::vector<Ffmpeg::PreviewResult> results = Ffmpeg::generatePreviewFrames(jobs, frameCount, qMax(1, QThread::idealThreadCount() / 3),
		[&](int done, int total) {
			progressBox.setText(tr("Generating preview %1/%2...").arg(done).arg(total));
			QApplication::processEvents();
		});

	for (size_t i = 0; i < jobs.size(); ++i)
		stageCard(jobs[i].videoFilePath, jobs[i].destinationFolder, results[i].durationMs);
}

void ImportDialog::unstage(const MediaId& id)
{
	auto it = m_staged.find(id);
	if (it == m_staged.end())
		return;

	if (!it->tempPreviewDir.isEmpty())  // photos have none; QDir("") would be the working directory
		QDir(it->tempPreviewDir).removeRecursively();
	delete it->item;  // also removes the row from m_stagedGrid and deletes its embedded MediaItemWidget
	m_staged.erase(it);
}

void ImportDialog::updateCardLabelDots(const MediaId& id)
{
	const auto it = m_staged.constFind(id);
	if (it == m_staged.constEnd())
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

	auto* card = static_cast<MediaItemWidget*>(m_stagedGrid->itemWidget(it->item));
	card->setLabelDots(colors, names.join(", "));
}

std::vector<MediaId> ImportDialog::effectiveStagedSelection(const MediaId& id) const
{
	const QList<QListWidgetItem*> selected = m_stagedGrid->selectedItems();
	if (selected.size() <= 1 || !selected.contains(m_staged.value(id).item))
		return { id };

	std::vector<MediaId> targets;
	for (auto it = m_staged.constBegin(); it != m_staged.constEnd(); ++it)
		if (selected.contains(it->item))
			targets.push_back(it.key());
	return targets;
}

void ImportDialog::showStagedCardContextMenu(const MediaId& id, const QPoint& globalPos)
{
	if (!m_staged.contains(id))
		return;
	const std::vector<MediaId> selection = effectiveStagedSelection(id);

	QMenu menu(this);

	// Compare photos: synchronized zoom/pan over the staged photo files themselves (as in MainWindow), offered for
	// an all-photo selection of 2+ - the file paths are ready even though nothing has been imported yet.
	std::vector<MediaId> photoSelection;
	for (const MediaId& sel : selection)
		if (isSupportedImageFile(m_staged.value(sel).path))
			photoSelection.push_back(sel);
	if (photoSelection.size() == selection.size() && photoSelection.size() >= 2)
	{
		menu.addAction(tr("Compare photos"), this, [this, photoSelection] { compareStagedPhotos(photoSelection); });
		menu.addSeparator();
	}

	// Single-target file actions act on the right-clicked card (like MainWindow's), not the whole selection.
	const bool isPhoto = isSupportedImageFile(m_staged.value(id).path);
	menu.addAction(isPhoto ? tr("Open photo") : tr("Play source video"), this, [this, id] { previewStagedItem(id); });
	menu.addAction(tr("Locate source file"), this, [this, id] { locateStagedSourceFile(id); });
	menu.addAction(tr("Copy source path to clipboard"), this, [this, id] { copyStagedSourcePath(id); });
	// Deferred: rebuilding the card (setItemWidget) deletes the current one, which is still on the stack here.
	QAction* renameItem = menu.addAction(tr("Rename..."), this, [this, id] {
		QMetaObject::invokeMethod(this, [this, id] { renameStagedItem(id); }, Qt::QueuedConnection);
	});
	renameItem->setShortcut(QKeySequence(Shortcuts::Rename));
	renameItem->setShortcutContext(Qt::WidgetShortcut);   // display-only; the real accelerator is the grid action
	menu.addSeparator();

	// Add to / Remove from Best across the whole selection: a uniform toggle (remove when all already carry it, else
	// add to all), the same shape the Labels checklist below uses.
	const bool allBest = std::all_of(selection.cbegin(), selection.cend(),
		[this](const MediaId& sel) { return m_staged.value(sel).pendingBest; });
	menu.addAction(allBest ? tr("Remove from Best") : tr("Add to Best"), this, [this, selection, allBest] {
		setBestForStagedSelection(selection, !allBest);
	});

	// Labels checklist - toggles the staged cards' pendingLabelIds instead of the Catalog (nothing is written there
	// until "Import" runs). Each row's color-tinted checkbox reflects the whole staged selection; toggling makes it
	// uniform (strip when all carry it, else add to all).
	std::vector<LabelVisuals::ChecklistRow> labelRows;
	for (const LabelOption& option : m_labelOptions)
	{
		int haveCount = 0;
		for (const MediaId& sel : selection)
			if (m_staged.value(sel).pendingLabelIds.contains(option.id))
				++haveCount;

		labelRows.push_back({ option.displayName, QColor(option.color),
			LabelVisuals::presenceForCount(haveCount, static_cast<int>(selection.size())),
			[this, selection, labelId = option.id](bool addToAll) {
				for (const MediaId& target : selection)
				{
					auto it = m_staged.find(target);
					if (it == m_staged.end())
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
	LabelVisuals::buildChecklistMenu(menu.addMenu(tr("Labels")), std::move(labelRows));
	menu.addSeparator();

	// Remove from staging (no disk change) and Delete (removes the source from disk). Both act on the effective staged
	// selection and display the same accelerators as the grid actions set up in the constructor. Deferred via a queued
	// call: this menu was opened from the card's own context-menu handler, still on the stack, and both actions delete
	// that very card - the mutation must wait until this unwinds (same reason MainWindow defers a card's own mutation).
	QAction* removeItem = menu.addAction(tr("Remove from staging"), this, [this, selection] {
		QMetaObject::invokeMethod(this, [this, selection] { removeStagedItems(selection); }, Qt::QueuedConnection);
	});
	removeItem->setShortcut(QKeySequence(Shortcuts::RemoveFromList));
	removeItem->setShortcutContext(Qt::WidgetShortcut);   // display-only in the menu; the real accelerator is the grid action

	QAction* deleteItem = menu.addAction(tr("Delete source file(s)"), this, [this, selection] {
		QMetaObject::invokeMethod(this, [this, selection] { deleteStagedSourceFiles(selection); }, Qt::QueuedConnection);
	});
	deleteItem->setShortcut(QKeySequence(Shortcuts::DeleteFile));
	deleteItem->setShortcutContext(Qt::WidgetShortcut);

	menu.exec(globalPos);
}

std::vector<MediaId> ImportDialog::selectedStagedIds() const
{
	const QList<QListWidgetItem*> selected = m_stagedGrid->selectedItems();
	std::vector<MediaId> ids;
	for (auto it = m_staged.constBegin(); it != m_staged.constEnd(); ++it)
		if (selected.contains(it->item))
			ids.push_back(it.key());
	return ids;
}

void ImportDialog::previewStagedItem(const MediaId& id)
{
	const auto it = m_staged.constFind(id);
	if (it == m_staged.constEnd())
		return;
	if (isSupportedImageFile(it->path))
		QDesktopServices::openUrl(QUrl::fromLocalFile(it->path));  // a photo opens in the system image viewer
	else
		VideoPlayerWindow::createPlayerWindow(m_library, it->path, this);
}

void ImportDialog::locateStagedSourceFile(const MediaId& id)
{
	const auto it = m_staged.constFind(id);
	if (it == m_staged.constEnd())
		return;

	if (!openInExplorer(it->path))
		reportMissingFile(this, it->path);
}

void ImportDialog::copyStagedSourcePath(const MediaId& id)
{
	const auto it = m_staged.constFind(id);
	if (it != m_staged.constEnd())
		QApplication::clipboard()->setText(QDir::toNativeSeparators(it->path));
}

void ImportDialog::compareStagedPhotos(const std::vector<MediaId>& photoIds)
{
	QStringList paths;
	for (const MediaId& id : photoIds)
		paths << m_staged.value(id).path;
	PhotoCompareWindow::showForFiles(paths, this);
}

void ImportDialog::setBestForStagedSelection(const std::vector<MediaId>& ids, bool inBest)
{
	for (const MediaId& id : ids)
	{
		auto it = m_staged.find(id);
		if (it == m_staged.end())
			continue;
		it->pendingBest = inBest;
		static_cast<MediaItemWidget*>(m_stagedGrid->itemWidget(it->item))->setInBest(inBest);  // sync the card's star
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
		? tr("Permanently delete this source file from disk?\n\n%1").arg(QDir::toNativeSeparators(m_staged.value(ids.front()).path))
		: tr("Permanently delete %1 source files from disk? This cannot be undone.").arg(ids.size());
	if (QMessageBox::warning(this, tr("Delete source file(s)"), question,
			QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
		return;

	QStringList failed;
	for (const MediaId& id : ids)
	{
		const auto it = m_staged.constFind(id);
		if (it == m_staged.constEnd())
			continue;
		const QString path = it->path;
		if (QFile::remove(path))
			unstage(id);          // also removes the card and cleans a video's temp preview dir
		else
			failed << QDir::toNativeSeparators(path);   // leave it staged so the user still sees the failure
	}

	if (!failed.isEmpty())
		QMessageBox::warning(this, tr("Delete source file(s)"),
			tr("These files could not be deleted:\n\n%1").arg(failed.join("\n")));
}

void ImportDialog::renameStagedItem(const MediaId& id)
{
	const auto it = m_staged.constFind(id);
	if (it == m_staged.constEnd())
		return;

	const StagedEntry entry = it.value();   // snapshot; carried over to the re-keyed entry below
	const QFileInfo oldInfo(entry.path);
	const QString oldBase = oldInfo.completeBaseName();
	const QString suffix = oldInfo.suffix();   // kept fixed so the file stays the same type (and a valid MediaId)

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

	// Reject a clash with another staged item (same new name+size) or an existing on-disk file. A case-only change
	// resolves to the same file, so it isn't treated as an on-disk clash.
	if (newId != id && m_staged.contains(newId))
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

	// Re-key to the new identity and swap the card's widget in place (same grid item, so no reorder). The rebuilt
	// card's callbacks bind to newId, preserving the "staged key == current file's MediaId" invariant runImport relies on.
	StagedEntry renamed = entry;
	renamed.path = newPath;
	auto* card = buildStagedCard(newId, newPath, renamed.tempPreviewDir, renamed.durationMs);
	m_stagedGrid->setItemWidget(renamed.item, card);   // deletes the previous card
	m_staged.remove(id);
	m_staged.insert(newId, renamed);

	if (renamed.pendingBest)
		card->setInBest(true);
	updateCardLabelDots(newId);
}

void ImportDialog::materializeUsedProvisionalLabels()
{
	// Provisional labels reach the Catalog only here, at Import: create each one a staged item actually carries and
	// map its provisional id to the real one. A provisional whose name already matched a real label was reused
	// directly (never minted), so it doesn't appear here; a genuinely new one is created with the swatch it showed.
	QHash<QString, QString> provisionalToReal;  // provisional id -> real id ("" = creation failed)
	for (auto it = m_staged.constBegin(); it != m_staged.constEnd(); ++it)
	{
		for (const QString& labelId : it->pendingLabelIds)
		{
			if (isProvisionalId(labelId) && !provisionalToReal.contains(labelId))
			{
				const LabelOption* option = findLabelOption(labelId);
				provisionalToReal.insert(labelId,
					option ? m_callbacks.createLabelRequested(option->displayName, option->color) : QString());
			}
		}
	}

	if (provisionalToReal.isEmpty())
		return;

	// Drop the now-created provisionals; the Catalog lists them as real labels from here on, so a partial
	// Import that leaves items staged won't try to recreate them on a second run. Refreshed before the remap
	// below so the changed cards' re-derived dots resolve the new real ids.
	m_provisionalLabels.erase(std::remove_if(m_provisionalLabels.begin(), m_provisionalLabels.end(),
		[&](const LabelOption& o) { return !provisionalToReal.value(o.id).isEmpty(); }), m_provisionalLabels.end());
	refreshLabelList();

	// Rewrite every staged pick through the mapping (real ids pass through unchanged); a label that failed to
	// be created maps to empty and is dropped from the pick, leaving its item staged but that bit unlabeled.
	if (remapStagedLabelIds(provisionalToReal))
		QMessageBox::warning(this, tr("Import"),
			tr("Some labels could not be created (the name may be reserved or invalid); the affected items were left unlabeled and remain staged."));
}

void ImportDialog::importPhotoGroup(const QString& labelId, const std::vector<MediaId>& photoIds, Import::PhotoImportMode mode, ImportOutcome& outcome)
{
	QStringList photoPaths;
	photoPaths.reserve(photoIds.size());
	for (const MediaId& id : photoIds)
		photoPaths << m_staged.value(id).path;

	const std::vector<Import::PhotoResult> results = m_callbacks.importPhotosRequested(labelId, photoPaths, mode);
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
				.arg(QDir::toNativeSeparators(m_staged.value(id).path)),
				QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes)
		{
			const std::vector<Import::PhotoResult> retried =
				m_callbacks.importPhotosRequested(labelId, { m_staged.value(id).path }, Import::PhotoImportMode::Copy);
			if (!retried.empty())
				result = retried.front();
		}

		if (result.status != Import::PhotoStatus::Success)
			continue;  // failed or declined - stays staged with its labels intact (errors already reported by the host)

		const StagedEntry entry = m_staged.value(id);
		outcome.succeededIds.push_back(id);
		// Bookkeeping keys on the *registered* id: an owned-import auto-rename gives the imported copy
		// a new name, hence a new identity - the staged id no longer names anything in the catalog.
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
	if (m_library.catalog().storageFolderForLabel(labelIdFromString(labelId)).isEmpty())
	{
		QMessageBox::warning(this, tr("Import"),
			tr("This label does not have a safe storage path:\n%1").arg(label->displayName));
		return;
	}

	QStringList paths;
	QHash<MediaId, QString> stagedPreviewDirs;  // by id: survives relocation rewriting the paths below
	QHash<MediaId, qint64> stagedDurations;     // likewise by id: the duration probed while staging each video
	paths.reserve(videoIds.size());
	stagedPreviewDirs.reserve(videoIds.size());
	stagedDurations.reserve(videoIds.size());
	for (const MediaId& id : videoIds)
	{
		const StagedEntry entry = m_staged.value(id);
		paths << entry.path;
		stagedPreviewDirs.insert(id, entry.tempPreviewDir);
		stagedDurations.insert(id, entry.durationMs);
	}

	const SourceRelocation::BatchResult relocated = SourceRelocation::relocateIfNeeded(
		m_library, this, paths, relocateMode, m_relocateFolderEdit->text());
	m_callbacks.addMediaItemsRequested(labelId, relocated.toImport, stagedPreviewDirs, stagedDurations);

	for (const MediaId& id : videoIds)
	{
		const StagedEntry entry = m_staged.value(id);
		if (relocated.keepStaged.contains(entry.path))
			continue;  // relocation deferred (Cancel) - stays staged untouched, try again later

		if (relocated.skipped.contains(entry.path))
		{
			outcome.skippedIds.push_back(id);  // resolved as "don't import" (the destination copy stands in for it) - clear from staging
			continue;
		}

		// The file was copied/moved: the staged entry follows it, so if the import below failed or was
		// declined, a later retry starts from where the file actually is (a Move deleted the original).
		if (const QString newPath = relocated.relocatedTo.value(entry.path); !newPath.isEmpty())
			m_staged[id].path = newPath;

		if (!isTrackedUnderLabel(m_library.catalog(), id, labelId))
			continue;  // import declined/failed, or the id collided with an item tracked elsewhere - stays staged with its labels intact

		outcome.succeededIds.push_back(id);

		if (entry.pendingBest)
			outcome.bestItems.push_back(id);
		if (entry.pendingLabelIds.size() > 1)
			outcome.extraLabelAssignments.push_back(ExtraLabelAssignment{ id, entry.pendingLabelIds.mid(1) });
	}
}

void ImportDialog::runImport()
{
	materializeUsedProvisionalLabels();

	// Group labeled entries by the first label dropped on them - the only place that ordering matters. Keyed
	// by MediaId throughout (the stable m_staged key, captured while the source file still existed): a Move
	// relocation deletes the source from its staged path before the post-import bookkeeping runs, so
	// re-deriving a MediaId from the path then would fail.
	QHash<QString, std::vector<MediaId>> idsByLabelId;
	for (auto it = m_staged.constBegin(); it != m_staged.constEnd(); ++it)
		if (!it->pendingLabelIds.isEmpty())
			idsByLabelId[it->pendingLabelIds.constFirst()].push_back(it.key());

	if (idsByLabelId.isEmpty())
	{
		QMessageBox::information(this, tr("Import"), tr("No staged item has been labeled yet."));
		return;
	}

	const RelocateMode relocateMode = static_cast<RelocateMode>(m_relocateModeCombo->currentData().toInt());
	// The relocation mode doubles as the photo import mode: Copy/Move land the file in the library's
	// photo storage, "leave in place" means tracking it right where it is (a referenced photo).
	const Import::PhotoImportMode photoMode =
		relocateMode == RelocateMode::Copy ? Import::PhotoImportMode::Copy :
		relocateMode == RelocateMode::Move ? Import::PhotoImportMode::Move :
		                                     Import::PhotoImportMode::Reference;

	ImportOutcome outcome;
	for (auto it = idsByLabelId.constBegin(); it != idsByLabelId.constEnd(); ++it)
	{
		const LabelOption* option = findLabelOption(it.key());
		if (!option)
			continue;  // the label vanished from the catalog mid-session - skip the group rather than guess

		// Photos and videos take different apply paths: videos relocate (optionally) to the user-chosen folder
		// and extract into <storageFolder>/<baseName>/, photos land in the label's own photo dir (or are merely
		// referenced) via importPhotosRequested - so each group is split by type first.
		std::vector<MediaId> videoIds;
		std::vector<MediaId> photoIds;
		for (const MediaId& id : it.value())
			(isSupportedImageFile(m_staged.value(id).path) ? photoIds : videoIds).push_back(id);

		if (!photoIds.empty())
			importPhotoGroup(it.key(), photoIds, photoMode, outcome);
		if (!videoIds.empty())
			importVideoGroup(it.key(), videoIds, relocateMode, outcome);
	}

	// Flush the imported items' Best flags and extra-label picks. Items only reach these lists after their
	// import was confirmed, so "tracked in the catalog" is the guard against a stray id - uniform across
	// videos and photos (a photo has no per-item folder whose existence could stand in for it).
	if (!outcome.bestItems.empty() || !outcome.extraLabelAssignments.empty())
	{
		Catalog& catalog = m_library.catalog();
		Catalog::BatchScope batch(catalog);  // one store write for the whole flush instead of one per label
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

	// Repaint the host once with the fully-applied state - the dialog stays open, so the mid-Import refresh
	// that addMediaItemsRequested may have done only shows folder labels, not the Best/extra labels flushed just above.
	if (!outcome.succeededIds.empty() && m_callbacks.viewChanged)
		m_callbacks.viewChanged();
}
