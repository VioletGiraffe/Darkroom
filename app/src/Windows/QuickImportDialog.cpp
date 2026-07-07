#include "Windows/QuickImportDialog.h"
#include "Ffmpeg.h"
#include "UiComponents/ContentWidthListWidget.h"
#include "UiComponents/LabelMimeType.h"
#include "UiComponents/LabelVisuals.h"
#include "Settings.h"
#include "Theme/Icons.h"
#include "Theme/Theme.h"
#include "Utils.h"
#include "UiComponents/MediaItemWidget.h"
#include "Windows/VideoPlayerWindow.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QColor>
#include <QColorDialog>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
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

// A small filled circle, used as each label-list row's leading icon - the flat, non-interactive cousin of
// LabelRowDelegate's dot (no active/hover states to paint here, so a plain decoration icon suffices).
QIcon colorDotIcon(const QColor& color)
{
	QPixmap pixmap(12, 12);
	pixmap.fill(Qt::transparent);
	QPainter p(&pixmap);
	p.setRenderHint(QPainter::Antialiasing);
	p.setPen(Qt::NoPen);
	p.setBrush(color.isValid() ? color : QColor("#888888"));
	p.drawEllipse(1, 1, 10, 10);
	return QIcon(pixmap);
}

// A fresh, unique scratch directory for one staged video's temp preview frames.
QString uniqueTempPreviewDir()
{
	return QDir::tempPath() + "/darkroom_quickimport/" + QUuid::createUuid().toString(QUuid::Id128);
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
			QDirIterator it(path, QDir::Files, QDirIterator::Subdirectories);
			while (it.hasNext())
			{
				it.next();
				const QFileInfo fi = it.fileInfo();
				if (isSupportedVideoFile(fi.filePath()) || isSupportedImageFile(fi.filePath()))
					files.append({ fi.filePath(), base.relativeFilePath(fi.absolutePath()).replace('/', '-') });
			}
		}
		else if (isSupportedVideoFile(path) || isSupportedImageFile(path))
			files.append({ path, {} });
	}
	return files;
}

// ============================================================================
// Source video relocation - optionally copy/move the dropped source video
// file into a chosen folder when it's added to a collection, and import from
// the new location instead of the original.
// ============================================================================

enum class RelocateMode { LeaveInPlace, Copy, Move };

// Performs the actual copy/move; on failure, warns and falls back to leaving
// the file at srcPath so the caller can still import it from there.
[[nodiscard]] QString copyOrMove(QWidget* dialogParent, const QString& srcPath, const QString& destPath, bool isMove)
{
	const bool ok = isMove ? QFile::rename(srcPath, destPath) : QFile::copy(srcPath, destPath);
	if (!ok)
	{
		QMessageBox::warning(dialogParent, QObject::tr("Error"), QObject::tr("Failed to %1:\n%2\nto:\n%3")
			.arg(isMove ? QObject::tr("move") : QObject::tr("copy"), srcPath, destPath));
		return srcPath;
	}
	return destPath;
}

// ============================================================================
// FileCollisionDialog - shown when the relocation destination already has a
// file with the same name as the one being added.
//
// Not a plain QMessageBox because the "files differ" case offers Play buttons
// that must NOT close the dialog (they let the user preview both files before
// deciding), alongside the decision buttons that do.
// ============================================================================

class FileCollisionDialog final : public QDialog
{
public:
	enum class Result { Overwrite, Skip, SkipAndDelete, Cancel };

	FileCollisionDialog(const QString& stagedPath, const QString& destPath, bool isDuplicate, QWidget* parent)
		: QDialog(parent)
	{
		setWindowTitle(isDuplicate ? tr("Duplicate File Found") : tr("File Already Exists"));

		// WindowModal (not the exec()-default ApplicationModal) blocks only this
		// dialog's parent (QuickImportDialog) and up - it deliberately leaves sibling
		// top-level windows, such as the VideoPlayerWindow opened by the Play
		// buttons below, interactive.
		setWindowModality(Qt::WindowModal);

		QVBoxLayout* layout = new QVBoxLayout(this);

		QLabel* message = new QLabel(isDuplicate
			? tr("An identical file is already at the destination:\n\n%1\n\nIt won't be imported again. You can optionally delete the redundant staged copy:\n\n%2").arg(destPath, stagedPath)
			: tr("A different file with the same name already exists at the destination:\n\n%1\n\nOverwrite it with the staged file, skip importing this one, or cancel to leave it staged and decide later:\n\n%2").arg(destPath, stagedPath),
			this);
		message->setWordWrap(true);
		layout->addWidget(message);

		QHBoxLayout* buttonRow = new QHBoxLayout;

		if (!isDuplicate)
		{
			// Play buttons open a preview parented to the outer QuickImportDialog
			// (this dialog's parent), not to this dialog - this dialog is
			// destroyed as soon as Overwrite/Skip is clicked, which would
			// otherwise kill an in-progress preview along with it.
			QWidget* previewParent = parent;

			QPushButton* playStaged = new QPushButton(tr("Play Staged File"), this);
			connect(playStaged, &QPushButton::clicked, this, [previewParent, stagedPath] {
				auto* player = new VideoPlayerWindow(stagedPath, MediaId::fromFile(stagedPath), previewParent);
				player->show();
			});
			buttonRow->addWidget(playStaged);

			QPushButton* playExisting = new QPushButton(tr("Play Existing File"), this);
			connect(playExisting, &QPushButton::clicked, this, [previewParent, destPath] {
				auto* player = new VideoPlayerWindow(destPath, MediaId::fromFile(destPath), previewParent);
				player->show();
			});
			buttonRow->addWidget(playExisting);
		}

		buttonRow->addStretch(1);

		if (isDuplicate)
		{
			QPushButton* skip = new QPushButton(tr("Skip"), this);
			connect(skip, &QPushButton::clicked, this, [this] { m_result = Result::Skip; accept(); });
			buttonRow->addWidget(skip);

			QPushButton* skipDelete = new QPushButton(tr("Skip and Delete Duplicate"), this);
			connect(skipDelete, &QPushButton::clicked, this, [this] { m_result = Result::SkipAndDelete; accept(); });
			buttonRow->addWidget(skipDelete);
		}
		else
		{
			QPushButton* overwrite = new QPushButton(tr("Overwrite"), this);
			connect(overwrite, &QPushButton::clicked, this, [this] { m_result = Result::Overwrite; accept(); });
			buttonRow->addWidget(overwrite);

			QPushButton* skip = new QPushButton(tr("Skip"), this);
			connect(skip, &QPushButton::clicked, this, [this] { m_result = Result::Skip; accept(); });
			buttonRow->addWidget(skip);

			// Defer: import nothing, touch no file, and leave the entry staged so
			// the user can deal with the name clash later.
			QPushButton* cancel = new QPushButton(tr("Cancel"), this);
			connect(cancel, &QPushButton::clicked, this, [this] { m_result = Result::Cancel; accept(); });
			buttonRow->addWidget(cancel);
		}

		layout->addLayout(buttonRow);
	}

	[[nodiscard]] Result result() const { return m_result; }

private:
	// Cancel is the default so dismissing the dialog (Escape / window close) defers
	// rather than taking any action - the safe no-op for either flavor.
	Result m_result = Result::Cancel;
};

// Outcome of relocating one file. importPath empty => don't import it; of those, keepStaged true => the
// user deferred (Cancel), so leave it staged for a later retry, while keepStaged false => the collision was
// resolved as "don't import" (Skip / Skip and Delete), so the entry should be cleared from staging.
struct RelocationOutcome
{
	QString importPath;
	bool keepStaged = false;
};

// Resolves relocation (including any naming collision) for a single file.
// On a collision "Skip"/"Skip and Delete" the destination is treated as the
// already-catalogued copy and this item is not imported; "Cancel" additionally
// keeps it staged. File-operation *failures* fall back to importing from the
// original path, so an I/O error never silently drops a file the user wanted.
[[nodiscard]] RelocationOutcome performRelocation(QWidget* dialogParent, const QString& path, RelocateMode mode, const QString& destFolder)
{
	const QString destPath = destFolder + "/" + QFileInfo(path).fileName();
	const bool isMove = (mode == RelocateMode::Move);

	// Already at the destination - e.g. a retry after an earlier Copy/Move whose import was declined (the
	// staged entry follows the file, see runImport) - so there is nothing to relocate and, critically, no
	// "collision": without this the file would be compared against itself and offered up as a duplicate.
	if (QFileInfo(path) == QFileInfo(destPath))
		return { path, false };

	if (!QFile::exists(destPath))
		return { copyOrMove(dialogParent, path, destPath, isMove), false };

	// A name+size match (MediaId) is the cheap first gate; on that collision a full byte comparison confirms
	// a genuine duplicate, so the astronomically-rare same-name/same-size/different-content case is still
	// classified as "files differ". The && short-circuits the byte read when the MediaIds (sizes) differ.
	const bool isDuplicate = (MediaId::fromFile(path) == MediaId::fromFile(destPath)) && filesAreIdentical(path, destPath);
	FileCollisionDialog collisionDialog(path, destPath, isDuplicate, dialogParent);
	collisionDialog.exec();

	switch (collisionDialog.result())
	{
	case FileCollisionDialog::Result::Overwrite:
		if (!QFile::remove(destPath))
		{
			QMessageBox::warning(dialogParent, QObject::tr("Error"), QObject::tr("Failed to overwrite existing file:\n%1").arg(destPath));
			return { path, false }; // I/O failure - fall back to importing from the original
		}
		return { copyOrMove(dialogParent, path, destPath, isMove), false };

	case FileCollisionDialog::Result::SkipAndDelete:
		if (!QFile::remove(path))
			QMessageBox::warning(dialogParent, QObject::tr("Error"), QObject::tr("Failed to delete duplicate file:\n%1").arg(path));
		return { {}, false }; // not imported, removed from staging

	case FileCollisionDialog::Result::Skip:
		return { {}, false }; // not imported, removed from staging

	case FileCollisionDialog::Result::Cancel:
		break;
	}

	return { {}, true }; // deferred - not imported, kept staged
}

// Result of relocating a whole drop batch, reported per original path so the caller can update its staging
// state. toImport is what to hand to import. Each original path additionally lands in at most one of:
// keepStaged (the user deferred via Cancel - leave the entry staged untouched), skipped (collision resolved
// as "don't import" via Skip / Skip and Delete - clear the entry from staging), or relocatedTo (the file was
// actually copied/moved - point the staged entry at the new location, so a retry after a failed/declined
// import starts from where the file really is; a Move deletes the original).
struct BatchRelocation
{
	QStringList toImport;
	QStringList keepStaged;
	QStringList skipped;
	QHash<QString, QString> relocatedTo;
};

// Batch entry point - no-op when relocation is disabled. Validates the
// destination folder once per batch (rather than once per file) to avoid
// repeating the same warning for every dropped file.
[[nodiscard]] BatchRelocation relocateIfNeeded(QWidget* dialogParent, const QStringList& paths, RelocateMode mode, const QString& destFolder)
{
	if (mode == RelocateMode::LeaveInPlace)
		return { paths, {} };

	if (destFolder.isEmpty())
	{
		QMessageBox::warning(dialogParent, QObject::tr("Error"), QObject::tr("No destination folder is set for relocating source files - they will be left in their original location."));
		return { paths, {} };
	}
	if (!QDir{}.mkpath(destFolder))
	{
		QMessageBox::warning(dialogParent, QObject::tr("Error"), QObject::tr("Could not create or access destination folder:\n%1").arg(destFolder));
		return { paths, {} };
	}

	BatchRelocation result;
	result.toImport.reserve(paths.size());
	for (const QString& path : paths)
	{
		const RelocationOutcome outcome = performRelocation(dialogParent, path, mode, destFolder);
		if (!outcome.importPath.isEmpty())
		{
			result.toImport.append(outcome.importPath);
			if (outcome.importPath != path)
				result.relocatedTo.insert(path, outcome.importPath);
		}
		else if (outcome.keepStaged)
			result.keepStaged.append(path);
		else
			result.skipped.append(path);
	}
	return result;
}

} // namespace

QuickImportDialog::QuickImportDialog(Callbacks callbacks, const QString& suggestedRelocateFolder, QWidget* parent)
	: QDialog(parent)
	, m_callbacks(std::move(callbacks))
{
	setWindowTitle(tr("Quick Import"));
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
	const int savedRelocateMode = relocateSettings.value("quickImportDialog/relocateMode", int(RelocateMode::LeaveInPlace)).toInt();
	m_relocateModeCombo->setCurrentIndex(qMax(0, m_relocateModeCombo->findData(savedRelocateMode)));

	// Persisted choice wins; the caller's suggestion only seeds the field on first use.
	QString relocateFolder = relocateSettings.value("quickImportDialog/relocateFolder").toString();
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
	// Hover fill matches the main-window sidebar's active-row highlight (BackgroundSecondary + ControlRadius), for a consistent feel.
	m_labelList->setStyleSheet(QStringLiteral("QListWidget::item:hover { background-color: %1; border-radius: %2px; }")
		.arg(Theme::current().BackgroundSecondary).arg(Theme::ControlRadius));
	m_labelList->viewport()->installEventFilter(this);  // drives the row-drag gesture (see eventFilter)
	m_labelList->setContextMenuPolicy(Qt::CustomContextMenu);  // right-click a row to edit a provisional label
	connect(m_labelList, &QListWidget::customContextMenuRequested, this, &QuickImportDialog::showLabelListContextMenu);
	labelPaneLayout->addWidget(m_labelList, 1);

	QPushButton* addLabelButton = new QPushButton(tr("Create label"));
	addLabelButton->setObjectName("addLabelButton");
	addLabelButton->setIcon(Theme::tintedIcon(QStringLiteral(":/UI/icon_plus.svg"), &Theme::ThemeColors::TextPrimary));
	connect(addLabelButton, &QPushButton::clicked, this, [this] {
		const QString name = QInputDialog::getText(this, tr("New Label"), tr("Label name:")).trimmed();
		if (name.isEmpty())
			return;
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
	connect(importButton, &QPushButton::clicked, this, &QuickImportDialog::runImport);
	footer->addWidget(importButton);
	outerLayout->addLayout(footer);

	refreshLabelList();

	// Restore persisted window geometry and splitter position (no-ops on first run).
	restoreWindowGeometry(this, "quickImportDialog");
	const QByteArray splitterState = QSettings{}.value("quickImportDialog/splitter").toByteArray();
	if (!splitterState.isEmpty())
		m_splitter->restoreState(splitterState);
}

QuickImportDialog::~QuickImportDialog()
{
	saveWindowGeometry(this, "quickImportDialog");
	QSettings{}.setValue("quickImportDialog/splitter", m_splitter->saveState());
	QSettings{}.setValue("quickImportDialog/relocateMode", m_relocateModeCombo->currentData().toInt());
	QSettings{}.setValue("quickImportDialog/relocateFolder", m_relocateFolderEdit->text());

	// Best-effort: clean up whatever's still staged when the dialog closes (anything already unstaged or
	// successfully added already removed its own temp dir). The isEmpty guard is load-bearing: staged photos
	// have no temp dir, and QDir("") refers to the working directory - removeRecursively on it would be a disaster.
	for (const StagedEntry& entry : std::as_const(m_staged))
		if (!entry.tempPreviewDir.isEmpty())
			QDir(entry.tempPreviewDir).removeRecursively();
}

void QuickImportDialog::dragEnterEvent(QDragEnterEvent* event)
{
	if (event->mimeData()->hasUrls())
	{
		for (const QUrl& url : event->mimeData()->urls())
		{
			const QString path = url.toLocalFile();
			if (QFileInfo(path).isDir() || isSupportedVideoFile(path) || isSupportedImageFile(path))
			{
				event->acceptProposedAction();
				return;
			}
		}
	}
}

void QuickImportDialog::dropEvent(QDropEvent* event)
{
	// Files and folders both accepted; stageMediaItems expands any folder into the supported files under it.
	QStringList paths;
	for (const QUrl& url : event->mimeData()->urls())
	{
		QString path = url.toLocalFile();
		if (QFileInfo(path).isDir() || isSupportedVideoFile(path) || isSupportedImageFile(path))
			paths.push_back(std::move(path));
	}
	if (!paths.isEmpty())
		stageMediaItems(paths);
}

bool QuickImportDialog::eventFilter(QObject* watched, QEvent* event)
{
	if (watched != m_labelList->viewport())
		return QDialog::eventFilter(watched, event);

	switch (event->type())
	{
	case QEvent::MouseButtonPress:
	{
		auto* me = static_cast<QMouseEvent*>(event);
		m_labelDragHelper.mousePressed(me);
		m_labelPressedItem = m_labelList->itemAt(me->pos());
		break;
	}
	case QEvent::MouseMove:
	{
		if (!m_labelPressedItem)
			break;
		const QString labelId = m_labelPressedItem->data(kLabelIdRole).toString();
		auto* me = static_cast<QMouseEvent*>(event);
		const QPixmap rowPixmap = m_labelList->viewport()->grab(m_labelList->visualItemRect(m_labelPressedItem));
		const bool started = m_labelDragHelper.tryStartDrag(
			m_labelList->viewport(), me,
			[labelId] {
				auto* mime = new QMimeData();
				mime->setData(LabelMimeType, labelId.toUtf8());
				return mime;
			},
			Qt::CopyAction, rowPixmap);
		if (started)
		{
			m_labelPressedItem = nullptr;
			return true;
		}
		break;
	}
	case QEvent::MouseButtonRelease:
		m_labelPressedItem = nullptr;
		break;
	default:
		break;
	}

	return QDialog::eventFilter(watched, event);
}

void QuickImportDialog::refreshLabelList()
{
	// The list is this session's provisional labels (folder-derived or manually added) followed by the Catalog's
	// real ones. Provisionals lead so they're easy to review/rename before Import materializes them.
	m_labelOptions.clear();
	for (const LabelOption& provisional : m_provisionalLabels)
		m_labelOptions.push_back(provisional);
	for (const LabelOption& real : m_callbacks.getLabelOptions())
		m_labelOptions.push_back(real);

	m_labelList->clear();
	for (const LabelOption& option : m_labelOptions)
	{
		auto* item = new QListWidgetItem(colorDotIcon(QColor(option.color)),
			option.provisional ? tr("%1  (new)").arg(option.displayName) : option.displayName, m_labelList);
		item->setData(kLabelIdRole, option.id);
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

const QuickImportDialog::LabelOption* QuickImportDialog::findLabelOption(const QString& id) const
{
	for (const LabelOption& option : m_labelOptions)
		if (option.id == id)
			return &option;
	return nullptr;
}

bool QuickImportDialog::isProvisionalId(const QString& id)
{
	return id.startsWith(QLatin1String("new:"));
}

QString QuickImportDialog::findLabelIdByName(const QString& name, const QString& excludeId) const
{
	// Case-insensitive to match the (Windows) filesystem and Catalog's own label-name uniqueness rule.
	for (const LabelOption& option : m_labelOptions)
		if (option.id != excludeId && option.displayName.compare(name, Qt::CaseInsensitive) == 0)
			return option.id;
	return {};
}

QString QuickImportDialog::addProvisionalLabel(const QString& name)
{
	LabelOption option;
	option.id = QStringLiteral("new:%1").arg(m_provisionalSeq++);
	option.displayName = name;
	option.color = m_callbacks.generateLabelColor ? m_callbacks.generateLabelColor() : QString();
	option.provisional = true;
	m_provisionalLabels.push_back(option);
	m_labelOptions.push_back(option);  // keep the cached union live so a follow-up findLabelId/updateCardLabelDots sees it at once
	return option.id;
}

QString QuickImportDialog::ensureLabelForFolderName(const QString& name)
{
	if (name.isEmpty())
		return {};
	// A folder whose name already matches a label (real, or an earlier provisional from this same drop) reuses it -
	// "existing labels need no action", and two files from one folder share a single label.
	const QString existing = findLabelIdByName(name, {});
	return existing.isEmpty() ? addProvisionalLabel(name) : existing;
}

void QuickImportDialog::showLabelListContextMenu(const QPoint& pos)
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

void QuickImportDialog::renameProvisionalLabel(const QString& provisionalId)
{
	const auto option = std::find_if(m_provisionalLabels.begin(), m_provisionalLabels.end(),
		[&](const LabelOption& o) { return o.id == provisionalId; });
	if (option == m_provisionalLabels.end())
		return;

	bool ok = false;
	const QString newName = QInputDialog::getText(this, tr("Rename label"), tr("New name:"),
		QLineEdit::Normal, option->displayName, &ok).trimmed();
	if (!ok || newName.isEmpty() || newName == option->displayName)
		return;

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

void QuickImportDialog::setProvisionalLabelColor(const QString& provisionalId)
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

void QuickImportDialog::deleteProvisionalLabel(const QString& provisionalId)
{
	// Strip the label from every staged card that carried it, then drop it from the list.
	for (auto it = m_staged.begin(); it != m_staged.end(); ++it)
		if (it->pendingLabelIds.removeAll(provisionalId) > 0)
			updateCardLabelDots(it.key());

	m_provisionalLabels.erase(std::remove_if(m_provisionalLabels.begin(), m_provisionalLabels.end(),
		[&](const LabelOption& o) { return o.id == provisionalId; }), m_provisionalLabels.end());
	refreshLabelList();
}

void QuickImportDialog::mergeProvisionalInto(const QString& provisionalId, const QString& targetId)
{
	// Point every staged pick at the target and drop the now-merged provisional. Pre-import this is purely local -
	// nothing in the Catalog changes; it's a rewrite of the staged cards' pending assignments (the first id still
	// decides the import destination, so order is preserved and any resulting duplicate collapsed).
	for (auto it = m_staged.begin(); it != m_staged.end(); ++it)
	{
		if (!it->pendingLabelIds.contains(provisionalId))
			continue;
		QStringList remapped;
		for (const QString& id : std::as_const(it->pendingLabelIds))
		{
			const QString mapped = (id == provisionalId) ? targetId : id;
			if (!remapped.contains(mapped))
				remapped << mapped;
		}
		it->pendingLabelIds = remapped;
		updateCardLabelDots(it.key());
	}

	m_provisionalLabels.erase(std::remove_if(m_provisionalLabels.begin(), m_provisionalLabels.end(),
		[&](const LabelOption& o) { return o.id == provisionalId; }), m_provisionalLabels.end());
	refreshLabelList();
}

void QuickImportDialog::updateAllCardLabelDots()
{
	for (auto it = m_staged.constBegin(); it != m_staged.constEnd(); ++it)
		updateCardLabelDots(it.key());
}

void QuickImportDialog::addToStaging(const QStringList& paths)
{
	// Deferred so the dialog can paint before stageMediaItems() blocks on ffmpeg preview extraction.
	QMetaObject::invokeMethod(this, [this, paths] { stageMediaItems(paths); }, Qt::QueuedConnection);
}

void QuickImportDialog::stageMediaItems(const QStringList& paths)
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
	// it, mirroring performRelocation's gate-then-verify shape: identical content is a plain duplicate,
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
		const QString existingPath = m_callbacks.findAlreadyImportedDuplicatePhoto(path);
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
		if (const QString name = labelNameByPath.value(path); !name.isEmpty())
			labelIdByPath.insert(path, ensureLabelForFolderName(name));
	if (!labelIdByPath.isEmpty())
		refreshLabelList();

	const int frameCount = QSettings{}.value(Settings::PreviewFrameCount, Defaults::PreviewFrameCount).toInt();

	// Card thumbnail canvases mirror the main library grid: a photo is square, a video is a horizontal strip
	// sized to tile with `frameCount` photo cards, so mixed staged cards line up on one column grid.
	const QSize photoCanvas{ STAGED_CARD_IMAGE_HEIGHT, STAGED_CARD_IMAGE_HEIGHT };
	const QSize videoCanvas{ MediaItemWidget::videoCanvasWidthForTiling(STAGED_CARD_IMAGE_HEIGHT, frameCount, m_stagedGrid->spacing()), STAGED_CARD_IMAGE_HEIGHT };

	// Builds one staged card + entry; shared by the photo and video paths below, which differ only in the card
	// canvas, the preview images, the temp dir (photos have none) and the double-click action. labelIdByPath is
	// captured by reference - stageCard is only ever called synchronously from within this function.
	const auto stageCard = [this, &labelIdByPath](QSize canvasSize, const QString& path, const QStringList& previewPaths,
		const QString& tempPreviewDir, std::function<void()> onDoubleClick) {
		const MediaId id = MediaId::fromFile(path);

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
			std::move(onDoubleClick),
			[this, id](QPoint globalPos) { showStagedCardContextMenu(id, globalPos); },
			/* dynamicSizeHint */ false
		);

		// A label dropped here tags just this card, or the whole selection if this card is part of one.
		card->setOnLabelDropped([this, id](const QString& labelId) {
			for (const MediaId& target : stagedSelection(id))
			{
				auto it = m_staged.find(target);
				if (it != m_staged.end() && !it->pendingLabelIds.contains(labelId))
					it->pendingLabelIds << labelId;
				updateCardLabelDots(target);
			}
		});

		auto* item = new QListWidgetItem();
		item->setSizeHint(card->sizeHint());
		m_stagedGrid->addItem(item);
		m_stagedGrid->setItemWidget(item, card);

		m_staged.insert(id, StagedEntry{ path, tempPreviewDir, /*pendingBest*/ false, /*pendingLabelIds*/ {}, item });

		// Pre-assign the folder-derived label (the first/destination label), if this file came from a dropped folder.
		if (const QString labelId = labelIdByPath.value(path); !labelId.isEmpty())
		{
			m_staged[id].pendingLabelIds << labelId;
			updateCardLabelDots(id);
		}
	};

	// Photo cards decode the file itself - no extraction step, so they stage instantly.
	for (const QString& path : photoPaths)
		stageCard(photoCanvas, path, { path }, /*tempPreviewDir*/ {}, [this, path] {
			QDesktopServices::openUrl(QUrl::fromLocalFile(path));  // double-click previews in the system image viewer
		});

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
	// leaves headroom for ffmpeg's own internal threading and for the UI.
	Ffmpeg::generatePreviewFrames(jobs, frameCount, qMax(1, QThread::idealThreadCount() / 3),
		[&](int done, int total) {
			progressBox.setText(tr("Generating preview %1/%2...").arg(done).arg(total));
			QApplication::processEvents();
		});

	for (const Ffmpeg::PreviewJob& job : jobs)
	{
		const QString& path = job.videoFilePath;

		QDir previewDir(job.destinationFolder);
		QStringList previewPaths;
		for (const QString& file : previewDir.entryList(IMAGE_FILE_FILTERS, QDir::Files, QDir::Name))
			previewPaths << previewDir.filePath(file);

		stageCard(videoCanvas, path, previewPaths, job.destinationFolder, [this, path] {
			auto* player = new VideoPlayerWindow(path, MediaId::fromFile(path), this);
			player->show();
		});
	}
}

void QuickImportDialog::unstage(const MediaId& id)
{
	auto it = m_staged.find(id);
	if (it == m_staged.end())
		return;

	if (!it->tempPreviewDir.isEmpty())  // photos have none; QDir("") would be the working directory
		QDir(it->tempPreviewDir).removeRecursively();
	delete it->item;  // also removes the row from m_stagedGrid and deletes its embedded MediaItemWidget
	m_staged.erase(it);
}

void QuickImportDialog::updateCardLabelDots(const MediaId& id)
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

std::vector<MediaId> QuickImportDialog::stagedSelection(const MediaId& id) const
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

void QuickImportDialog::showStagedCardContextMenu(const MediaId& id, const QPoint& globalPos)
{
	if (!m_staged.contains(id))
		return;
	const std::vector<MediaId> selection = stagedSelection(id);

	QMenu menu(this);

	// Labels checklist - mirrors MainWindow's showMediaItemContextMenu, but toggles the staged cards' pendingLabelIds
	// instead of the Catalog (nothing is written there until "Import" runs). Each row's color-tinted checkbox
	// reflects the whole staged selection; toggling makes it uniform (strip when all carry it, else add to all).
	QMenu* labelsMenu = menu.addMenu(tr("Labels"));
	if (m_labelOptions.empty())
	{
		labelsMenu->addAction(tr("(no labels yet)"))->setEnabled(false);
	}
	else
	{
		for (const LabelOption& option : m_labelOptions)
		{
			int haveCount = 0;
			for (const MediaId& sel : selection)
				if (m_staged.value(sel).pendingLabelIds.contains(option.id))
					++haveCount;
			const LabelVisuals::Presence presence = LabelVisuals::presenceForCount(haveCount, static_cast<int>(selection.size()));

			QAction* action = labelsMenu->addAction(LabelVisuals::checkboxIcon(presence, QColor(option.color), labelsMenu), option.displayName);
			const bool addToAll = presence != LabelVisuals::Presence::All;
			connect(action, &QAction::triggered, this, [this, selection, labelId = option.id, addToAll] {
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
			});
		}
	}

	menu.addSeparator();
	// Deferred: this menu was opened from the card's own customContextMenuRequested handler, still on the
	// call stack here - unstage() deletes that very card, so the delete must happen after this unwinds
	// (same reason MainWindow defers a card's own label-drop mutation).
	menu.addAction(tr("Remove from staging"), this, [this, id] {
		QMetaObject::invokeMethod(this, [this, id] { unstage(id); }, Qt::QueuedConnection);
	});

	menu.exec(globalPos);
}

void QuickImportDialog::materializeUsedProvisionalLabels()
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
					option ? m_callbacks.createCollectionRequested(option->displayName, option->color) : QString());
			}
		}
	}

	if (provisionalToReal.isEmpty())
		return;

	// Rewrite every staged pick through the mapping (real ids pass through unchanged); a label that failed to be
	// created is dropped from the pick, leaving its item staged but that bit unlabeled.
	bool anyFailed = false;
	for (auto it = m_staged.begin(); it != m_staged.end(); ++it)
	{
		QStringList remapped;
		for (const QString& labelId : std::as_const(it->pendingLabelIds))
		{
			const QString mapped = provisionalToReal.value(labelId, labelId);
			if (mapped.isEmpty())
				anyFailed = true;
			else if (!remapped.contains(mapped))
				remapped << mapped;
		}

		it->pendingLabelIds = remapped;
	}

	// Drop the now-created provisionals; getLabelOptions() returns them as real labels from here on, so a partial
	// Import that leaves items staged won't try to recreate them on a second run.
	m_provisionalLabels.erase(std::remove_if(m_provisionalLabels.begin(), m_provisionalLabels.end(),
		[&](const LabelOption& o) { return !provisionalToReal.value(o.id).isEmpty(); }), m_provisionalLabels.end());
	refreshLabelList();
	updateAllCardLabelDots();  // any card left staged now reflects real ids (and drops a label that failed to create)

	if (anyFailed)
		QMessageBox::warning(this, tr("Import"),
			tr("Some labels could not be created (the name may be reserved or invalid); the affected items were left unlabeled and remain staged."));
}

void QuickImportDialog::runImport()
{
	materializeUsedProvisionalLabels();

	// Group labeled entries by the first label dropped on them - the only place that ordering matters. Keyed
	// by MediaId throughout (the stable m_staged key, captured while the source file still existed): a Move
	// relocation deletes the source from its staged path before the post-import bookkeeping below runs, so
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

	std::vector<MediaId> bestItems;
	std::vector<ExtraLabelAssignment> extraLabelAssignments;
	std::vector<MediaId> succeededIds;
	std::vector<MediaId> skippedIds;  // collision resolved as "don't import" - cleared from staging like a success, minus the label flush

	for (auto it = idsByLabelId.constBegin(); it != idsByLabelId.constEnd(); ++it)
	{
		const LabelOption* option = findLabelOption(it.key());
		if (!option)
			continue;  // the label vanished from the catalog mid-session - skip the group rather than guess

		// Photos and videos take different apply paths: videos relocate (optionally) to the user-chosen folder
		// and extract into <collection>/<baseName>/, photos land in the label's own photo dir (or are merely
		// referenced) via importPhotosRequested - so each group is split by type first.
		std::vector<MediaId> videoIds;
		std::vector<MediaId> photoIds;
		for (const MediaId& id : it.value())
			(isSupportedImageFile(m_staged.value(id).path) ? photoIds : videoIds).push_back(id);

		// --- Photos ---
		if (!photoIds.empty())
		{
			// The relocation mode doubles as the photo import mode: Copy/Move land the file in the library's
			// photo storage, "leave in place" means tracking it right where it is (a referenced photo).
			const Import::PhotoImportMode photoMode =
				relocateMode == RelocateMode::Copy ? Import::PhotoImportMode::Copy :
				relocateMode == RelocateMode::Move ? Import::PhotoImportMode::Move :
				                                     Import::PhotoImportMode::Reference;

			QStringList photoPaths;
			photoPaths.reserve(photoIds.size());
			for (const MediaId& id : photoIds)
				photoPaths << m_staged.value(id).path;

			const std::vector<Import::PhotoResult> results = m_callbacks.importPhotosRequested(it.key(), photoPaths, photoMode);
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
						m_callbacks.importPhotosRequested(it.key(), { m_staged.value(id).path }, Import::PhotoImportMode::Copy);
					if (!retried.empty())
						result = retried.front();
				}

				if (result.status != Import::PhotoStatus::Success)
					continue;  // failed or declined - stays staged with its labels intact (errors already reported by the host)

				const StagedEntry entry = m_staged.value(id);
				succeededIds.push_back(id);
				// Bookkeeping keys on the *registered* id: an owned-import auto-rename gives the imported copy
				// a new name, hence a new identity - the staged id no longer names anything in the catalog.
				if (entry.pendingBest)
					bestItems.push_back(result.registeredId);
				if (entry.pendingLabelIds.size() > 1)
					extraLabelAssignments.push_back(ExtraLabelAssignment{ result.registeredId, entry.pendingLabelIds.mid(1) });
			}
		}

		// --- Videos ---
		if (videoIds.empty())
			continue;

		QStringList paths;
		QHash<MediaId, QString> stagedPreviewDirs;  // by id: survives relocation rewriting the paths below
		paths.reserve(videoIds.size());
		stagedPreviewDirs.reserve(videoIds.size());
		for (const MediaId& id : videoIds)
		{
			const StagedEntry entry = m_staged.value(id);
			paths << entry.path;
			stagedPreviewDirs.insert(id, entry.tempPreviewDir);
		}

		const BatchRelocation relocated = relocateIfNeeded(this, paths, relocateMode, m_relocateFolderEdit->text());
		m_callbacks.addMediaItemsRequested(option->displayName, relocated.toImport, stagedPreviewDirs);

		for (const MediaId& id : videoIds)
		{
			const StagedEntry entry = m_staged.value(id);
			if (relocated.keepStaged.contains(entry.path))
				continue;  // relocation deferred (Cancel) - stays staged untouched, try again later

			if (relocated.skipped.contains(entry.path))
			{
				skippedIds.push_back(id);  // resolved as "don't import" (the destination copy stands in for it) - clear from staging
				continue;
			}

			// The file was copied/moved: the staged entry follows it, so if the import below failed or was
			// declined, a later retry starts from where the file actually is (a Move deleted the original).
			if (const QString newPath = relocated.relocatedTo.value(entry.path); !newPath.isEmpty())
				m_staged[id].path = newPath;

			if (!m_callbacks.isMediaItemTrackedInCollection(id, option->displayName))
				continue;  // import declined/failed, or the id collided with an item tracked elsewhere - stays staged with its labels intact

			succeededIds.push_back(id);

			if (entry.pendingBest)
				bestItems.push_back(id);
			if (entry.pendingLabelIds.size() > 1)
				extraLabelAssignments.push_back(ExtraLabelAssignment{ id, entry.pendingLabelIds.mid(1) });
		}
	}

	if (m_callbacks.markBestRequested && !bestItems.empty())
		m_callbacks.markBestRequested(bestItems);
	if (m_callbacks.assignExtraLabelsRequested && !extraLabelAssignments.empty())
		m_callbacks.assignExtraLabelsRequested(extraLabelAssignments);

	for (const MediaId& id : succeededIds)
		unstage(id);
	for (const MediaId& id : skippedIds)
		unstage(id);

	// Repaint the host once with the fully-applied state - the dialog stays open, so the mid-Import refresh
	// that addMediaItemsRequested may have done only shows folder labels, not the Best/extra labels flushed just above.
	if (!succeededIds.empty() && m_callbacks.viewChanged)
		m_callbacks.viewChanged();
}
