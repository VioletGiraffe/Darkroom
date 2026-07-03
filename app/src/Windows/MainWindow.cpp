#include "Windows/MainWindow.h"
#include "Core/Catalog.h"
#include "Windows/CompareWindow.h"
#include "Windows/PhotoCompareWindow.h"
#include "Ffmpeg.h"
#include "Import.h"
#include "Windows/FrameViewerWindow.h"
#include "Windows/FindUntrackedFilesDialog.h"
#include "Windows/IntegrityCheckDialog.h"
#include "UiComponents/LabelSidebar.h"
#include "UiComponents/LabelVisuals.h"
#include "Windows/QuickImportDialog.h"
#include "UiComponents/SegmentedToggle.h"
#include "Windows/SettingsDialog.h"
#include "UiComponents/SortControl.h"
#include "Core/MediaId.h"
#include "UiComponents/MediaItemWidget.h"
#include "Windows/VideoPlayerWindow.h"
#include "Theme/Theme.h"
#include "Utils.h"
#include "Settings.h"

#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QColorDialog>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QProcess>
#include <QScrollBar>
#include <QSet>
#include <QSettings>
#include <QShortcut>
#include <QSplitter>
#include <QTimer>
#include <QVBoxLayout>

#include <QScopeGuard>

#include <algorithm>
#include <assert.h>
#include <functional>
#include <utility>
#include <vector>

static MainWindow* s_instance = nullptr;

// Per-frame preview image height (px); card width is this × the frame count. User-adjustable via Ctrl+wheel
// over a card, persisted under a settings key local to this UI (not hoisted into the shared Settings.h).
static const QString CARD_IMAGE_HEIGHT_KEY = "mainWindow/cardImageHeight";
// The header's All/Videos/Photos switch, persisted as its segment index (0 = All).
static const QString MEDIA_TYPE_FILTER_KEY = "mainWindow/mediaTypeFilter";
static constexpr int DEFAULT_CARD_IMAGE_HEIGHT = 120;
static constexpr int MIN_CARD_IMAGE_HEIGHT = 60;
static constexpr int MAX_CARD_IMAGE_HEIGHT = 360;
static constexpr int CARD_IMAGE_HEIGHT_STEP = 20;

// Preview frames per card. User-selectable (1–10) via the header combobox. The settings key/default live in
// Settings.h (Settings::PreviewFrameCount) since QuickImportDialog's staged cards mirror this choice too.
static constexpr int MIN_PREVIEW_FRAME_COUNT = 1;
static constexpr int MAX_PREVIEW_FRAME_COUNT = 10;

// Must match Catalog's reserved Best label displayName (see Catalog::ensureBestLabelExists) - Best can never
// be renamed, so this can't drift. A stale "★ Best" (the pre-Catalog tab name) here would let a label
// literally named "Best" be created through this guard, colliding in the sidebar with the real virtual one.
static const QString BEST_COLLECTION_NAME = "Best";

inline bool    useTiff()     { return QSettings{}.value(Settings::UseTiff,      Defaults::UseTiff).toBool(); }
inline int     jpegQuality() { return QSettings{}.value(Settings::JpegQuality,  Defaults::JpegQuality).toInt(); }
inline int     frameStep()   { return QSettings{}.value(Settings::FrameStep,    Defaults::FrameStep).toInt(); }
inline int     cardImageHeight() { return QSettings{}.value(CARD_IMAGE_HEIGHT_KEY, DEFAULT_CARD_IMAGE_HEIGHT).toInt(); }

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent)
{
	s_instance = this;

	setWindowTitle("Darkroom");
	resize(1500, 800);
	setAcceptDrops(true);

	m_frameViewer = new FrameViewerWindow();

	setupUI();
	backfillMissingPreviews();

	// The one initial grid build is deliberately deferred to restoreSettings() below (queued): it applies the
	// persisted label filter and calls refreshMediaGrid() once. Building here too would construct every card
	// twice on startup - once with the default filter, then again with the restored one.
	QMetaObject::invokeMethod(this, [this] {
		restoreSettings();
	}, Qt::QueuedConnection);
}

MainWindow::~MainWindow()
{
	delete m_frameViewer;
	s_instance = nullptr;
	saveSettings();
}

MainWindow* MainWindow::instance()
{
	return s_instance;
}

void MainWindow::setupUI()
{
	auto* central = new QWidget(this);
	setCentralWidget(central);

	auto* rootLayout = new QHBoxLayout(central);
	rootLayout->setContentsMargins(0, 0, 0, 0);
	rootLayout->setSpacing(0);

	// Left: the label sidebar (filter), replacing the old per-collection tab bar.
	m_labelSidebar = new LabelSidebar();
	connect(m_labelSidebar, &LabelSidebar::filterChanged, this, &MainWindow::refreshMediaGrid);
	connect(m_labelSidebar, &LabelSidebar::addLabelRequested, this, &MainWindow::createLabelInteractive);
	connect(m_labelSidebar, &LabelSidebar::renameLabelRequested, this, &MainWindow::renameLabelInteractive);
	connect(m_labelSidebar, &LabelSidebar::setLabelColorRequested, this, &MainWindow::setLabelColorInteractive);
	connect(m_labelSidebar, &LabelSidebar::deleteLabelRequested, this, &MainWindow::deleteLabelInteractive);

	// Right: a toolbar of view controls above the card grid.
	auto* rightPanel = new QWidget();
	auto* mainLayout = new QVBoxLayout(rightPanel);
	mainLayout->setContentsMargins(0, 0, 0, 0);
	mainLayout->setSpacing(0);

	auto* headerWidget = new QWidget();
	auto* headerLayout = new QHBoxLayout(headerWidget);
	headerLayout->setContentsMargins(0, 4, 4, 4);
	headerLayout->setSpacing(4);
	// Name filter on the left: a folder-name substring filter ANDed with the sidebar's label filter.
	// Carries the former SearchDialog's query syntax (see nameMatchesFilter). textChanged only hides/shows
	// the already-built cards (applyNameFilter) - no catalog rebuild, no thumbnail re-decode - so it stays
	// cheap on every keystroke.
	m_nameFilter = new QLineEdit();
	m_nameFilter->setPlaceholderText(tr("filter by name..."));
	m_nameFilter->setClearButtonEnabled(true);
	m_nameFilter->setMinimumWidth(220);
	connect(m_nameFilter, &QLineEdit::textChanged, this, &MainWindow::applyNameFilter);
	headerLayout->addWidget(m_nameFilter, 0, Qt::AlignVCenter);

	// Media-type filter beside the name filter: All / Videos / Photos, ANDed with the other filters. A
	// structural filter (changes which items are enumerated), so it rebuilds the grid, unlike the name
	// filter's cheap hide/show. setCurrentIndex is silent, so restoring the persisted choice here doesn't
	// fire a redundant refresh during setup.
	m_mediaTypeFilter = new SegmentedToggle({ tr("All"), tr("Videos"), tr("Photos") });
	m_mediaTypeFilter->setCurrentIndex(qBound(0, QSettings{}.value(MEDIA_TYPE_FILTER_KEY, 0).toInt(), 2));
	connect(m_mediaTypeFilter, &SegmentedToggle::currentChanged, this, [this](int index) {
		QSettings{}.setValue(MEDIA_TYPE_FILTER_KEY, index);
		refreshMediaGrid();
	});
	headerLayout->addWidget(m_mediaTypeFilter, 0, Qt::AlignVCenter);

	// Ctrl+F focuses the name filter (app-wide, like the menu shortcuts) instead of opening a search dialog.
	// Surface the main window first, since the shortcut can fire while a player/frame-viewer window is on top.
	auto* focusFilterShortcut = new QShortcut(QKeySequence("Ctrl+F"), this);
	focusFilterShortcut->setContext(Qt::ApplicationShortcut);
	connect(focusFilterShortcut, &QShortcut::activated, this, [this] {
		raise();
		activateWindow();
		m_nameFilter->setFocus();
		m_nameFilter->selectAll();
	});

	headerLayout->addStretch(1);  // view controls sit on the right of the stretch, the name filter on the left

	// Preview-frame-count selector. A drop-down is quicker to pick from than spinning a QSpinBox, and
	// self-labelling items ("N frames per preview") replace the usual separate QLabel + control pair.
	m_previewFrameCountCombo = new QComboBox();
	m_previewFrameCountCombo->setToolTip(tr("Number of preview frames shown on each video card"));
	for (int n = MIN_PREVIEW_FRAME_COUNT; n <= MAX_PREVIEW_FRAME_COUNT; ++n)
		m_previewFrameCountCombo->addItem((n == 1 ? tr("%1 frame per preview") : tr("%1 frames per preview")).arg(n), n);

	// Apply the saved value before connecting, so restoring it during setup doesn't fire a redundant refresh.
	const int savedFrameCount = QSettings{}.value(Settings::PreviewFrameCount, Defaults::PreviewFrameCount).toInt();
	const int savedFrameIdx = m_previewFrameCountCombo->findData(savedFrameCount);
	m_previewFrameCountCombo->setCurrentIndex(savedFrameIdx >= 0 ? savedFrameIdx
		: m_previewFrameCountCombo->findData(Defaults::PreviewFrameCount));

	connect(m_previewFrameCountCombo, &QComboBox::currentIndexChanged, this, [this] {
		QSettings{}.setValue(Settings::PreviewFrameCount, m_previewFrameCountCombo->currentData().toInt());
		refreshMediaGrid();
	});
	headerLayout->addWidget(m_previewFrameCountCombo, 0, Qt::AlignVCenter);

	// Sort control: one chip-button showing the current field + direction; clicking opens a popover with
	// every ordering option (field, direction, favorites-first). It owns its own persistence and just tells
	// us when the order changed - the sort itself stays in resortMediaGrid(). Favorites-first is a no-op
	// while viewing the Best tab (everything there is already a favorite, so its partition collapses).
	m_sortControl = new SortControl();
	connect(m_sortControl, &SortControl::changed, this, &MainWindow::resortMediaGrid);
	headerLayout->addWidget(m_sortControl, 0, Qt::AlignVCenter);

	mainLayout->addWidget(headerWidget);

	// Media item card grid
	m_mediaGrid = new QListWidget();
	m_mediaGrid->setViewMode(QListView::IconMode);
	m_mediaGrid->setFlow(QListView::LeftToRight);
	m_mediaGrid->setWrapping(true);
	m_mediaGrid->setResizeMode(QListView::Adjust);
	// Every card in a refresh is built to the same size (shared card-image height x preview-frame count, plus a
	// fixed footer), so the view can skip per-item size accounting during layout - a large win when populating
	// hundreds of items, since otherwise each addItem re-measures the wrapping grid. Precondition: keep cards
	// uniformly sized; revisit if variable-size cards are ever introduced.
	m_mediaGrid->setUniformItemSizes(true);
	m_mediaGrid->setMovement(QListView::Static);
	m_mediaGrid->setSelectionMode(QAbstractItemView::ExtendedSelection);
	// Without this, the view reads "button-held + mouse moving" as a rubber-band reselection, which collapses an
	// existing multi-selection to the single item under the cursor before our custom card drag can even start.
	// With drag enabled, the view defers the collapse (it's expecting a drag), so a multi-selection survives the
	// press+move long enough for ThumbnailWidget's QDrag to take over. The view itself never starts a drag — our
	// ThumbnailWidget consumes the threshold-crossing move first.
	m_mediaGrid->setDragEnabled(true);
	m_mediaGrid->setSpacing(10);
	m_mediaGrid->setStyleSheet(QStringLiteral("QListWidget::item:selected { background-color: %1; }").arg(Theme::current().AccentBg));

	mainLayout->addWidget(m_mediaGrid, 1);

	// Ctrl+wheel over a card adjusts the preview height; coalesce a burst of wheel steps into one rebuild.
	m_gridZoomDebounce = new QTimer(this);
	m_gridZoomDebounce->setSingleShot(true);
	m_gridZoomDebounce->setInterval(80);
	connect(m_gridZoomDebounce, &QTimer::timeout, this, &MainWindow::refreshMediaGrid);

	// Assemble the window as a resizable [sidebar | right panel] split.
	auto* splitter = new QSplitter(Qt::Horizontal);
	splitter->addWidget(m_labelSidebar);
	splitter->addWidget(rightPanel);
	splitter->setStretchFactor(0, 0);
	splitter->setStretchFactor(1, 1);
	splitter->setCollapsible(0, false);
	rootLayout->addWidget(splitter);

	setupMainMenu();
}

void MainWindow::setupMainMenu()
{
	auto* menuBar = new QMenuBar(this);
	setMenuBar(menuBar);

	QMenu* fileMenu = new QMenu(tr("File"), menuBar);
	fileMenu->addAction(tr("Settings..."), QKeySequence("Ctrl+Alt+P"), this, &MainWindow::openSettings);
	fileMenu->addSeparator();
	fileMenu->addAction(tr("Exit"), QKeySequence("Ctrl+Q"), this, &QMainWindow::close);

	QMenu* toolsMenu = new QMenu(tr("Tools"), menuBar);
	toolsMenu->addAction(tr("Quick import to collections..."), QKeySequence("Ctrl+Shift+A"), this, [this] { quickImportToCollections(); });
	toolsMenu->addAction(tr("Scan for untracked files..."), this, &MainWindow::scanForUntrackedFiles);
	toolsMenu->addAction(tr("Check catalog integrity..."), this, &MainWindow::checkCatalogIntegrity);
	toolsMenu->addSeparator();
	toolsMenu->addAction(tr("Restart all videos"), QKeySequence("Shift+R"), this, &VideoPlayerWindow::restartAll);
	toolsMenu->addAction(tr("Close all videos"),   QKeySequence("Shift+W"), this, &VideoPlayerWindow::closeAll);
	toolsMenu->addSeparator();
	toolsMenu->addAction(tr("Re-export all videos"), QKeySequence("Ctrl+Shift+E"), this, &MainWindow::reExportAllVideos);

	menuBar->addMenu(fileMenu);
	menuBar->addMenu(toolsMenu);

	for (QAction* action : menuBar->actions())
	{
		if (!action->menu())
			continue;
		for (QAction* sub : action->menu()->actions())
			sub->setShortcutContext(Qt::ApplicationShortcut);
	}
}

void MainWindow::saveSettings()
{
	saveWindowGeometry(this, "mainWindow");
	QSettings{}.setValue("mainWindow/activeLabelIds", m_labelSidebar->activeLabelIds());
	QSettings{}.setValue("mainWindow/labelsAndMode", m_labelSidebar->isAndMode());
}

void MainWindow::restoreSettings()
{
	restoreWindowGeometry(this, "mainWindow");

	const QStringList activeIds = QSettings{}.value("mainWindow/activeLabelIds").toStringList();
	const bool andMode = QSettings{}.value("mainWindow/labelsAndMode", false).toBool();
	m_labelSidebar->setActiveFilter(activeIds, andMode);  // silent; the grid refresh below applies it
	refreshMediaGrid();
}

void MainWindow::openSettings()
{
	SettingsDialog dialog(this);
	connect(&dialog, &CSettingsDialog::settingsChanged, this, &MainWindow::refreshLibraryView);
	dialog.exec();
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event)
{
	if (event->mimeData()->hasUrls())
	{
		for (const QUrl& url : event->mimeData()->urls())
		{
			const QString path = url.toLocalFile();
			if (isSupportedVideoFile(path) || isSupportedImageFile(path))
			{
				event->acceptProposedAction();
				return;
			}
		}
	}
}

void MainWindow::dropEvent(QDropEvent* event)
{
	QStringList files;
	for (const QUrl& url : event->mimeData()->urls())
	{
		QString path = url.toLocalFile();
		if (isSupportedVideoFile(path) || isSupportedImageFile(path))
			files.push_back(std::move(path));
	}

	// There is no "current collection" in the label model, so hand the dropped files to Quick Import, where the
	// user picks the destination label(s). Deferred so the drop source application is released promptly
	// instead of being held in the drag state until processing finishes.
	QMetaObject::invokeMethod(this, [this, files = std::move(files)] {
		quickImportToCollections(files);
	}, Qt::QueuedConnection);
}

void MainWindow::closeEvent(QCloseEvent* event)
{
	VideoPlayerWindow::closeAll();
	QMainWindow::closeEvent(event);
}

void MainWindow::refreshLibraryView()
{
	refreshMediaGrid();          // rebuilds the card grid from the (already-current) catalog model
	m_labelSidebar->refresh();   // pull the now-current labels/counts into the sidebar
}

void MainWindow::zoomCards(int steps)
{
	const int current = cardImageHeight();
	const int next = qBound(MIN_CARD_IMAGE_HEIGHT, current + steps * CARD_IMAGE_HEIGHT_STEP, MAX_CARD_IMAGE_HEIGHT);
	if (next == current)
		return;

	QSettings{}.setValue(CARD_IMAGE_HEIGHT_KEY, next);
	m_gridZoomDebounce->start(); // rebuild once the wheel settles
}

namespace {

// One grid card's sort keys, computed once per (re)build/resort so GridItem::operator< below does
// no disk I/O (date is a possible filesystem stat).
struct ItemInfo {
	bool isBest = false;
	QDateTime date; // resolved only when sorting by date
	QString name;
};

// The display name + sort keys for one item, shared by the grid build and the in-place resort. A video is
// named after its frame folder; a photo has none that names it (an owned photo's folder is the shared label
// dir, a referenced photo's is empty), so its name comes from the id's file name.
ItemInfo itemInfoFor(Catalog& catalog, const MediaId& id, bool isBest, bool sortByDate)
{
	const QString folderPath = catalog.folderForMediaItem(id);
	const QString name = catalog.mediaType(id) == Catalog::MediaType::Photo
		? QFileInfo(id.name()).completeBaseName()
		: QFileInfo(folderPath).fileName();
	return { isBest, sortByDate ? getSourceFileDate(catalog.sourcePathForMediaItem(id), folderPath) : QDateTime{}, name };
}

// A grid item that orders itself by its bundled ItemInfo. The sort mode (sortBy/descending/
// favoritesFirst) is the same for every item during a given sort, so it's set on these static
// members just before sortItems() runs rather than duplicated into each item's info. sortItems()
// carries each item's setItemWidget() card along with it, so a resort reorders the existing cards
// instead of tearing them down and re-decoding them.
class GridItem final : public QListWidgetItem {
public:
	static int sortBy;
	static bool descending;
	static bool favoritesFirst;

	MediaId mediaId;  // the card's identity; the grid is enumerated and addressed by this, not by folder path
	ItemInfo info;

	bool operator<(const QListWidgetItem& other) const override {
		const ItemInfo& b = static_cast<const GridItem&>(other).info;
		if (favoritesFirst && info.isBest != b.isBest)
			return info.isBest;
		if (sortBy == SortBy::Date)
			return descending ? b.date < info.date : info.date < b.date;

		const int cmp = QString::compare(info.name, b.name, Qt::CaseInsensitive);
		return descending ? cmp > 0 : cmp < 0;
	}
};
int GridItem::sortBy = SortBy::Name;
bool GridItem::descending = false;
bool GridItem::favoritesFirst = false;

// The "N:  name" caption shown on each card, where N is its 1-based display position. name is the card's
// display name (the frame-folder name), already resolved into the GridItem's info.
QString gridCaption(int displayNumber, const QString& name)
{
	return QString::number(displayNumber) + ":  " + name;
}

// Computes and applies one card's colored label-dot overlay from the catalog's current label set for
// that item, including Best - the star button stays as a fast one-click toggle, but the dot strip still
// shows Best so the card's label display is uniform across all labels.
void applyLabelDots(Catalog& catalog, const MediaId& id, MediaItemWidget* card)
{
	std::vector<QColor> dotColors;
	QStringList dotNames;
	for (const QString& labelId : catalog.labelsForMediaItem(id))
	{
		const Catalog::Label* label = catalog.labelById(labelId);
		if (!label)
			continue;
		dotColors.push_back(label->color.isEmpty() ? QColor() : QColor(label->color));
		dotNames << label->displayName;
	}
	card->setLabelDots(dotColors, dotNames.join(", "));
}

// Numbers the "N:" caption of each *visible* card 1..M in row order, after a sort reorders them or the
// name filter hides some. Hidden cards are skipped so the visible numbering stays contiguous (a hidden
// card keeps its stale caption, which is corrected the next time it's shown and renumbered).
void renumberGridCaptions(QListWidget* grid)
{
	int visibleNumber = 0;
	for (int row = 0; row < grid->count(); ++row)
	{
		QListWidgetItem* item = grid->item(row);
		if (item->isHidden())
			continue;
		auto* card = static_cast<MediaItemWidget*>(grid->itemWidget(item));
		card->setLabel(gridCaption(++visibleNumber, static_cast<GridItem*>(item)->info.name));
	}
}

// Toolbar name-filter query syntax (carried over from the removed SearchDialog): a leading '^' anchors
// the match to the start of the name, otherwise it matches anywhere; always case-insensitive; an empty
// query matches everything.
bool nameMatchesFilter(const QString& name, const QString& query)
{
	if (query.isEmpty())
		return true;
	if (query.startsWith('^'))
		return name.startsWith(query.mid(1), Qt::CaseInsensitive);
	return name.contains(query, Qt::CaseInsensitive);
}

} // namespace

void MainWindow::backfillMissingPreviews()
{
	Catalog& catalog = Catalog::instance();
	std::vector<MediaId> toBackfill;
	for (const MediaId& id : catalog.allMediaItems())
	{
		if (catalog.mediaType(id) != Catalog::MediaType::Video)
			continue;  // photo cards decode the photo file itself - no preview/ cache exists or is wanted

		if (!catalog.isSplitIntoFrames(id))
			continue;  // not-yet-split videos already got a real preview/ at import time

		const QString previewFolder = Catalog::previewDirFor(catalog.folderForMediaItem(id));
		if (QDir(previewFolder).entryList(IMAGE_FILE_FILTERS, QDir::Files).isEmpty())
			toBackfill.push_back(id);
	}

	if (toBackfill.empty())
		return;

	const int previewFrameCount = m_previewFrameCountCombo->currentData().toInt();

	QMessageBox progressBox(this);
	progressBox.setWindowTitle(tr("Updating previews"));
	progressBox.setStandardButtons(QMessageBox::NoButton);
	progressBox.setModal(true);
	progressBox.show();

	for (size_t i = 0; i < toBackfill.size(); ++i)
	{
		progressBox.setText(tr("Updating preview %1/%2...").arg(i + 1).arg(toBackfill.size()));
		QApplication::processEvents();
		// A genuine ghost (folder emptied externally) has no real frames, so regenerate returns false and skips
		// it - CatalogIntegrity::scan handles that case, not us.
		regeneratePreviewFromRealFrames(catalog.folderForMediaItem(toBackfill[i]), previewFrameCount);
	}
}

bool MainWindow::regeneratePreviewFromRealFrames(const QString& folderPath, int frameCount)
{
	QDir folderDir(folderPath);
	const QStringList realFrames = folderDir.entryList(IMAGE_FILE_FILTERS, QDir::Files, QDir::Name);
	if (realFrames.isEmpty())
		return false;

	const QString previewFolder = Catalog::previewDirFor(folderPath);
	QDir{}.mkpath(previewFolder);
	for (const QString& sourceFrame : pickEvenlySpacedFrames(folderDir, realFrames, frameCount))
		QFile::copy(sourceFrame, previewFolder + "/" + QFileInfo(sourceFrame).fileName());
	return true;
}

bool MainWindow::regeneratePreviewFor(const MediaId& id)
{
	Catalog& catalog = Catalog::instance();
	const QString folder = catalog.folderForMediaItem(id);
	const int frameCount = m_previewFrameCountCombo->currentData().toInt();

	// Prefer the video's own real frames - a plain copy, no source needed. Their presence also means the entry
	// is genuinely split whatever its flag said, so reconcile that (clears a co-occurring STALE flag).
	if (regeneratePreviewFromRealFrames(folder, frameCount))
	{
		catalog.markSplitComplete(id);
		return true;
	}

	// No real frames (a not-yet-split video): re-extract the preview straight from the source, if present.
	const QString source = catalog.sourcePathForMediaItem(id);
	if (!QFile::exists(source))
		return false;

	Ffmpeg::generatePreviewFrames(source, Catalog::previewDirFor(folder), frameCount);
	return !QDir(Catalog::previewDirFor(folder)).entryList(IMAGE_FILE_FILTERS, QDir::Files).isEmpty();
}

void MainWindow::refreshMediaGrid()
{
	m_mediaGrid->clear();

	const int previewFrameCount = m_previewFrameCountCombo->currentData().toInt();
	const int imageHeight = cardImageHeight();

	// The catalog is the authoritative in-memory model, kept current by its mutations, so the grid reads it
	// directly - no per-refresh re-derivation. Look up the Best set once: the per-card star state and the
	// favorites-first sort below share it.
	Catalog& catalog = Catalog::instance();
	const QSet<MediaId> bestSet = catalog.mediaItemsForLabel(Catalog::BestLabelId);

	GridItem::favoritesFirst = m_sortControl->favoritesFirst();
	GridItem::sortBy = m_sortControl->sortBy();
	GridItem::descending = m_sortControl->descending();
	const bool sortByDate = GridItem::sortBy == SortBy::Date;

	// Card widgets are attached in a second pass (after the addItem loop and sortItems), not inline: interleaving
	// setItemWidget with addItem makes each insert's endInsertRows walk every persistent editor index created so
	// far -> O(N^2). This list carries each item and its not-yet-attached card between the two passes.
	std::vector<std::pair<GridItem*, MediaItemWidget*>> pendingAttach;

	// Every card in a refresh is the same size (fixed size-hint mode; shared image height x frame count, plus a
	// constant footer), so the hint is computed once from the first built card and reused - the same uniformity
	// setUniformItemSizes(true) relies on. Querying all 850 would needlessly activate each card's layout.
	QSize uniformCardHint;

	// Cards are added in catalog order with their sort info attached, then sortItems() (using
	// GridItem::operator<) puts them in their final order; captions are numbered after that.
	const auto addCard = [&](const MediaId& id) {
		const QString folderPath = catalog.folderForMediaItem(id);
		const bool isPhoto = catalog.mediaType(id) == Catalog::MediaType::Photo;
		QStringList previewPaths;
		if (isPhoto)
		{
			// A photo card decodes the photo file itself - no preview cache (v1). An unloadable path (e.g. a
			// referenced photo on an unmounted drive) renders a blank card rather than hiding the item.
			previewPaths << catalog.sourcePathForMediaItem(id);
		}
		else
		{
			// Video cards always render from the permanent preview/ subfolder, never the real frame folder
			// directly - this is what lets a not-yet-split video (no real frames yet) still show a real thumbnail.
			QDir previewDir(Catalog::previewDirFor(folderPath));
			const QStringList imageFiles = previewDir.entryList(IMAGE_FILE_FILTERS, QDir::Files, QDir::Name);
			if (imageFiles.isEmpty())
				return;  // preview/ missing or empty (externally deleted folder, or preview generation failed outright) - don't show a frameless ghost card

			previewPaths = pickEvenlySpacedFrames(previewDir, imageFiles, previewFrameCount);
		}

		auto* card = new MediaItemWidget(
			QSize{ imageHeight * previewFrameCount, imageHeight },
			previewPaths, QString(),
			id,
			bestSet.contains(id),
			[this, id] { toggleBestFolder(id); },
			// double-click: a video opens in the built-in player, a photo in the system image viewer
			[this, id, isPhoto] { if (isPhoto) openSourceInSystemApp(id); else playVideo(id); },
			[this, id](QPoint globalPos) { showMediaItemContextMenu(id, globalPos); },
			/* dynamic size hint */false
		);
		if (!isPhoto)  // the frame viewer browses a video's frame folder; a photo has no frames, so no middle-click
		{
			card->setOnMiddleButtonClick([this, id, folderPath] {
				ensureFramesSplit(id);  // transparently runs the full split first, if this video hasn't had one yet
				m_frameViewer->showForFolder(folderPath);
			});
		}
		card->setOnMouseWheelCallback([this](int steps) { zoomCards(steps); });
		card->setSplitPending(!catalog.isSplitIntoFrames(id));

		// A label dragged from the sidebar onto this card is added to it (or to the whole selection if this card
		// is part of it). Mirrors the context-menu "Labels" add path; drag only ever adds, never removes.
		// Deferred: refreshLibraryView rebuilds the grid, deleting this very card mid-dropEvent, so the mutation
		// runs after the drop event unwinds (same reason MainWindow::dropEvent defers Quick Import).
		card->setOnLabelDropped([this, id](const QString& labelId) {
			const std::vector<MediaId> targets = effectiveSelection(id);
			QMetaObject::invokeMethod(this, [this, targets, labelId] {
				Catalog::BatchScope batch;  // one store write for the whole selection instead of one per item
				for (const MediaId& target : targets)
					Catalog::instance().addLabel(target, labelId);
				refreshLibraryView();
			}, Qt::QueuedConnection);
		});

		applyLabelDots(catalog, id, card);

		auto* item = new GridItem();
		item->mediaId = id;
		item->info = itemInfoFor(catalog, id, bestSet.contains(id), sortByDate);
		if (!uniformCardHint.isValid())
			uniformCardHint = card->sizeHint();
		item->setSizeHint(uniformCardHint);
		m_mediaGrid->addItem(item);

		pendingAttach.emplace_back(item, card);   // widget attached in the second pass, after sortItems()
	};

	// Media items to show = the sidebar's active label filter applied to the catalog (final order comes from
	// sortItems() below, so the enumeration order here doesn't matter).
	const QStringList activeLabelIds = m_labelSidebar->activeLabelIds();
	std::vector<MediaId> mediaItems;
	if (activeLabelIds.isEmpty())
	{
		mediaItems = catalog.allMediaItems();  // no filter ("All"): the whole catalog
	}
	else
	{
		QSet<MediaId> matched = catalog.mediaItemsForLabel(activeLabelIds.first());
		for (qsizetype i = 1; i < activeLabelIds.size(); ++i)
		{
			const QSet<MediaId> next = catalog.mediaItemsForLabel(activeLabelIds[i]);
			if (m_labelSidebar->isAndMode())
				matched.intersect(next);   // AND: items carrying every selected label
			else
				matched.unite(next);       // OR: items carrying any selected label
		}
		mediaItems.assign(matched.cbegin(), matched.cend());
	}

	// The header's All/Videos/Photos switch, ANDed with the label filter above.
	if (const int typeFilterIdx = m_mediaTypeFilter->currentIndex(); typeFilterIdx != 0)
	{
		const Catalog::MediaType wanted = typeFilterIdx == 2 ? Catalog::MediaType::Photo : Catalog::MediaType::Video;
		std::erase_if(mediaItems, [&](const MediaId& id) { return catalog.mediaType(id) != wanted; });
	}

	pendingAttach.reserve(mediaItems.size());

	for (const MediaId& id : mediaItems)   // pass 1: build cards + insert bare items (editor-free, so O(N))
		addCard(id);

	m_mediaGrid->sortItems(Qt::AscendingOrder);   // sorts bare items, no editors to reposition -> cheap

	// Pass 2: attach the card widgets, now that all items exist and are sorted. Interleaving this with the
	// addItem loop above made each insert's endInsertRows walk every persistent editor index created so far
	// -> O(N^2); deferring it keeps the inserts editor-free.
	for (const auto& [item, card] : pendingAttach)
		m_mediaGrid->setItemWidget(item, card);

	// The name filter is a view-level hide/show over these cards (applied here too so a structural rebuild
	// keeps honouring the active filter); renumberGridCaptions runs inside applyNameFilter.
	applyNameFilter();
}

// Reorders the existing cards in place to match the current sort controls, without rebuilding any
// widgets (which would re-decode every thumbnail). Each card's ItemInfo is refreshed from the catalog
// via its stored MediaId, then GridItem::operator< does the ordering via sortItems() - each item's
// setItemWidget() card moves with it through the model's persistent indexes.
void MainWindow::resortMediaGrid()
{
	const int count = m_mediaGrid->count();
	if (count == 0)
		return;

	Catalog& catalog = Catalog::instance();
	const QSet<MediaId> bestSet = catalog.mediaItemsForLabel(Catalog::BestLabelId);

	GridItem::favoritesFirst = m_sortControl->favoritesFirst();
	GridItem::sortBy = m_sortControl->sortBy();
	GridItem::descending = m_sortControl->descending();
	const bool sortByDate = GridItem::sortBy == SortBy::Date;

	for (int row = 0; row < count; ++row)
	{
		auto* item = static_cast<GridItem*>(m_mediaGrid->item(row));
		item->info = itemInfoFor(catalog, item->mediaId, bestSet.contains(item->mediaId), sortByDate);
	}

	m_mediaGrid->sortItems(Qt::AscendingOrder);
	renumberGridCaptions(m_mediaGrid);
}

void MainWindow::applyNameFilter()
{
	const QString query = m_nameFilter->text().trimmed();
	for (int row = 0; row < m_mediaGrid->count(); ++row)
	{
		auto* item = static_cast<GridItem*>(m_mediaGrid->item(row));
		const bool hide = !nameMatchesFilter(item->info.name, query);
		item->setHidden(hide);
		if (hide)
			item->setSelected(false);  // don't leave a hidden card in the selection (effectiveSelection walks it)
	}
	renumberGridCaptions(m_mediaGrid);
}

std::vector<MediaId> MainWindow::effectiveSelection(const MediaId& id) const
{
	std::vector<MediaId> selected;
	for (const QListWidgetItem* item : m_mediaGrid->selectedItems())
		selected.push_back(static_cast<const GridItem*>(item)->mediaId);
	if (std::find(selected.cbegin(), selected.cend(), id) == selected.cend())
		selected = { id };
	return selected;
}

void MainWindow::showMediaItemContextMenu(const MediaId& id, const QPoint& globalPos)
{
	Catalog& catalog = Catalog::instance();
	const std::vector<MediaId> selection = effectiveSelection(id);
	const QString folderPath = catalog.folderForMediaItem(id);
	const bool isPhoto = catalog.mediaType(id) == Catalog::MediaType::Photo;

	QMenu menu(this);

	// CompareWindow browses frame folders, so it's videos-only (photos get their own PhotoCompareWindow
	// below) - offered only when nothing in the selection is a photo.
	const bool selectionAllVideos = std::all_of(selection.cbegin(), selection.cend(),
		[&catalog](const MediaId& sel) { return catalog.mediaType(sel) == Catalog::MediaType::Video; });
	if (selectionAllVideos)
	{
		menu.addAction(selection.size() > 1 ? tr("Compare selected") : tr("Inspect"), [this, selection] {
			QStringList folders;
			for (const MediaId& sel : selection)
				folders << Catalog::instance().folderForMediaItem(sel);
			auto* w = new CompareWindow(folders, this);
			w->setAttribute(Qt::WA_DeleteOnClose);
			w->show();
		});
		menu.addSeparator();
	}

	// PhotoCompareWindow: synchronized zoom/pan over the photo files themselves - offered for a small
	// all-photo selection (2..4; a bigger grid stops being a useful comparison).
	const bool selectionAllPhotos = std::all_of(selection.cbegin(), selection.cend(),
		[&catalog](const MediaId& sel) { return catalog.mediaType(sel) == Catalog::MediaType::Photo; });
	if (selectionAllPhotos && selection.size() >= 2)
	{
		menu.addAction(tr("Compare photos"), [this, selection] {
			QStringList paths;
			static constexpr size_t MaxImages = 50;
			for (const MediaId& sel : selection)
			{
				const QString path = Catalog::instance().sourcePathForMediaItem(sel);
				if (!path.isEmpty() && QFileInfo::exists(path))
				{
					paths.push_back(path);
					if (paths.size() >= MaxImages)
						break;
				}
			}

			if (paths.size() < 2)
			{
				QMessageBox::warning(this, tr("Error"), tr("The selected photo files could not be found on disk."));
				return;
			}
			auto* w = new PhotoCompareWindow(paths, this);
			w->setAttribute(Qt::WA_DeleteOnClose);
			w->show();
		});
		menu.addSeparator();
	}

	// A photo's folderForMediaItem is the shared Photos/<label> dir (or nothing when referenced), not a
	// folder of its own to open - "Locate source file" below is how to reach the photo itself.
	if (!isPhoto)
	{
		menu.addAction(tr("Open in Explorer"), [folderPath] {
			openInExplorer(folderPath);
		});
	}
	menu.addAction(isPhoto ? tr("Open photo") : tr("Play source video"), [this, id] {
		openSourceInSystemApp(id);
	});
	menu.addAction(tr("Locate source file"), [this, id] {
		const QString sourcePath = Catalog::instance().sourcePathForMediaItem(id);
		if (sourcePath.isEmpty())
		{
			QMessageBox::warning(this, tr("Error"), tr("No source file is recorded for this item."));
			return;
		}
		if (!QFileInfo::exists(sourcePath))  // openInExplorer silently no-ops on a missing path, so guard+report here
		{
			reportMissingFile(this, sourcePath);
			return;
		}
		openInExplorer(sourcePath);
	});
	menu.addAction(tr("Copy source path to clipboard"), [id] {
		const QString sourcePath = Catalog::instance().sourcePathForMediaItem(id);
		if (!sourcePath.isEmpty())
			QApplication::clipboard()->setText(QDir::toNativeSeparators(sourcePath));
	});
	// Rename renames the frame folder + source file as one unit - video-shaped paths, no photo support (v1).
	if (!isPhoto)
	{
		menu.addAction(tr("Rename media file"), [this, id] {
			renameMediaItemInteractive(id);
		});
	}
	menu.addSeparator();

	const bool inBest = isInBest(id);
	menu.addAction(inBest ? tr("Remove from Best") : tr("Add to Best"), [this, id] {
		toggleBestFolder(id);
	});

	// Labels submenu: a checklist of every ordinary label. Each row's color-tinted checkbox reflects the whole
	// effective selection - checked when every selected item has the label, a dash when only some do, an empty box
	// when none do. Toggling makes the selection uniform: strip the label when all already carry it, else add to all.
	QMenu* labelsMenu = menu.addMenu(tr("Labels"));
	{
		bool anyLabel = false;
		for (const Catalog::Label& label : catalog.allLabels())
		{
			if (label.isVirtual())  // Best is the star / "Add to Best" action above, not a dot/checkbox
				continue;
			anyLabel = true;
			const QString labelId = label.id;

			int haveCount = 0;
			for (const MediaId& sel : selection)
				if (catalog.mediaItemHasLabel(sel, labelId))
					++haveCount;
			const LabelVisuals::Presence presence = LabelVisuals::presenceForCount(haveCount, static_cast<int>(selection.size()));

			QAction* action = labelsMenu->addAction(LabelVisuals::checkboxIcon(presence, QColor(label.color), labelsMenu), label.displayName);
			const bool addToAll = presence != LabelVisuals::Presence::All;
			connect(action, &QAction::triggered, this, [this, selection, labelId, addToAll] {
				Catalog::BatchScope batch;  // one store write for the whole selection instead of one per item
				for (const MediaId& target : selection)
				{
					if (addToAll)
						Catalog::instance().addLabel(target, labelId);
					else
						Catalog::instance().removeLabel(target, labelId);
				}
				refreshLibraryView();
			});
		}
		if (!anyLabel)
			labelsMenu->addAction(tr("(no labels yet)"))->setEnabled(false);
	}
	menu.addSeparator();

	menu.addAction(selection.size() > 1 ? tr("Delete all (%1 items)").arg(selection.size()) : tr("Delete all"), [this, selection] {
		Catalog& catalog = Catalog::instance();

		// What "delete" means per type: a video loses its frame folder and source file; an owned photo loses
		// its file ONLY - its folderForMediaItem is the shared Photos/<label> dir, home to sibling photos,
		// and must never be deleted; a referenced photo is only dropped from the catalog, its file untouched.

		// The confirmation lists what goes. A single item: its exact paths. A multi-selection: the count plus
		// the item names (capped, so a huge selection stays readable).
		QString message;
		if (selection.size() == 1)
		{
			const MediaId& sel = selection.front();
			const QString sourcePath = catalog.sourcePathForMediaItem(sel);
			if (catalog.isReferenced(sel))
			{
				message = tr("This will remove the item from the catalog:\n\n• %1\n\nThe file itself will not be touched.").arg(sourcePath);
			}
			else if (catalog.mediaType(sel) == Catalog::MediaType::Photo)
			{
				message = tr("This will permanently delete:\n\n• %1").arg(sourcePath);
			}
			else
			{
				message = tr("This will permanently delete:\n\n• %1").arg(catalog.folderForMediaItem(sel));
				if (!sourcePath.isEmpty())
					message += "\n• " + sourcePath;
			}
		}
		else
		{
			bool anyVideo = false, anyOwnedPhoto = false, anyReferencedPhoto = false;
			for (const MediaId& sel : selection)
			{
				if (catalog.mediaType(sel) == Catalog::MediaType::Video)
					anyVideo = true;
				else if (catalog.isReferenced(sel))
					anyReferencedPhoto = true;
				else
					anyOwnedPhoto = true;
			}

			QStringList deletedKinds;
			if (anyVideo)
				deletedKinds << tr("each video's frame folder and source file");
			if (anyOwnedPhoto)
				deletedKinds << tr("each owned photo's file");
			if (!deletedKinds.isEmpty())
				message = tr("This will permanently delete %1 items - %2:\n").arg(selection.size()).arg(deletedKinds.join(", "));
			else  // nothing but referenced photos selected - no file is deleted at all
				message = tr("This will remove %1 items from the catalog - their files will not be touched:\n").arg(selection.size());

			constexpr size_t maxListed = 15;
			for (size_t i = 0; i < std::min(maxListed, selection.size()); ++i)
			{
				const MediaId& sel = selection[i];
				// A video is named by its frame folder; a photo's folder is shared/empty, so use the id's file name.
				message += "\n• " + (catalog.mediaType(sel) == Catalog::MediaType::Photo
					? sel.name() : QFileInfo(catalog.folderForMediaItem(sel)).fileName());
			}
			if (selection.size() > maxListed)
				message += "\n" + tr("... and %1 more").arg(selection.size() - maxListed);

			if (anyReferencedPhoto && !deletedKinds.isEmpty())
				message += "\n\n" + tr("Referenced photos in the selection are only removed from the catalog - their files are not touched.");
		}
		message += tr("\n\nThis cannot be undone. Continue?");

		if (QMessageBox::warning(this, tr("Delete all"), message,
			QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
			return;

		Catalog::BatchScope batch;  // one store write for the whole selection instead of one per item
		for (const MediaId& sel : selection)
		{
			const QString sourcePath = catalog.sourcePathForMediaItem(sel);
			if (catalog.mediaType(sel) == Catalog::MediaType::Photo)
			{
				if (!catalog.isReferenced(sel))
					QFile::remove(sourcePath);
				catalog.removeMediaItem(sel);
				continue;
			}

			const QString folderPath = catalog.folderForMediaItem(sel);
			deleteFolderRecursively(folderPath);
			if (!sourcePath.isEmpty())
				QFile::remove(sourcePath);
			catalog.removeMediaItem(sel);  // drop it from the catalog so it doesn't linger as a ghost

			if (m_frameViewer->currentFolder() == folderPath)
				m_frameViewer->showForFolder({});
		}

		refreshLibraryView();
	});

	menu.exec(globalPos);
}

void MainWindow::playVideo(const MediaId& id)
{
	const QString sourcePath = Catalog::instance().sourcePathForMediaItem(id);
	if (!QFile::exists(sourcePath))
	{
		reportMissingFile(this, sourcePath);
		return;
	}

	auto* playerWindow = new VideoPlayerWindow(sourcePath, id, nullptr);  // hand the player the catalog id directly
	playerWindow->show();
}

void MainWindow::openSourceInSystemApp(const MediaId& id)
{
	const QString sourcePath = Catalog::instance().sourcePathForMediaItem(id);
	if (!QFile::exists(sourcePath))
	{
		reportMissingFile(this, sourcePath);
		return;
	}

	QDesktopServices::openUrl(QUrl::fromLocalFile(sourcePath));
}

void MainWindow::resplitVideoIntoFrames(const QString& videoFilePath, const QString& outputFolder, bool preserveExistingPreview)
{
	const QString previewDir = Catalog::previewDirFor(outputFolder);
	const QString preservedPreviewDir = outputFolder + "_preview_preserve";
	const bool hasPreview = preserveExistingPreview && QDir(previewDir).exists();
	if (hasPreview)
		QDir().rename(previewDir, preservedPreviewDir);

	deleteFolderRecursively(outputFolder);
	splitVideoIntoFrames(videoFilePath, outputFolder);

	if (hasPreview)
	{
		QDir{}.mkpath(outputFolder);
		QDir().rename(preservedPreviewDir, previewDir);
	}
	else if (!QDir(outputFolder).entryList(IMAGE_FILE_FILTERS, QDir::Files).isEmpty())
	{
		// Not preserving an old preview (re-export/reimport want a fresh one) - regenerate it now, but only
		// if the full split actually produced real frames; a failed split already wiped outputFolder via
		// splitVideoIntoFrames's own cleanup, and a preview over no real content would be misleading.
		Ffmpeg::generatePreviewFrames(videoFilePath, Catalog::previewDirFor(outputFolder), m_previewFrameCountCombo->currentData().toInt());
	}
}

void MainWindow::ensureFramesSplit(const MediaId& id)
{
	Catalog& catalog = Catalog::instance();
	if (catalog.isSplitIntoFrames(id))
		return;

	// The folder already holds preview/ frames from import, freshly generated and still valid - preserve
	// them across the wipe rather than redoing that work (unlike re-export/reimport, nothing changed that
	// would make them stale).
	resplitVideoIntoFrames(catalog.sourcePathForMediaItem(id), catalog.folderForMediaItem(id), /*preserveExistingPreview=*/true);
}

void MainWindow::splitVideoIntoFrames(const QString& videoFilePath, const QString& outputFolder)
{
	// Check the source is actually there before doing anything: otherwise ffmpeg fails on the missing input
	// and the user gets its raw stderr dump instead of a clear "the file is gone" message.
	if (!QFileInfo::exists(videoFilePath))
	{
		reportMissingFile(this, videoFilePath);
		return;
	}

	const QString baseName = QFileInfo(videoFilePath).completeBaseName();

	if (!QDir{}.mkpath(outputFolder))
	{
		QMessageBox::critical(this, tr("Error"), tr("Failed to create output folder:\n%1").arg(outputFolder));
		return;
	}

	// Removes outputFolder on any failure path below. Callers always delete it before calling
	// this function, so it's safe to wipe wholesale - this just undoes the mkpath above plus
	// whatever ffmpeg partially wrote, so a failed extraction doesn't leave debris behind.
	auto cleanupAfterFailure = [&outputFolder] { QDir(outputFolder).removeRecursively(); };

	// Build ffmpeg command
	const bool tiff = useTiff();
	const QString outputPattern = outputFolder + "/%04d_" + baseName + (tiff ? ".tif" : ".jpg");

	QStringList arguments;
	arguments << "-i" << QDir::toNativeSeparators(videoFilePath)
		<< "-an" << "-sn" << "-dn" // No audio, no subtitles, no data
		<< "-y"; // Overwrite output files without asking

	const int step = frameStep();
	if (step > 1)
	{
		// The comma inside mod(n,N) must be escaped: ffmpeg's filtergraph parser splits filters on
		// commas and doesn't track parentheses, so an unescaped one would truncate the select
		// expression. The comma before format= is left bare - it's the real select->format chain separator.
		arguments << "-filter:v" << QString("select=not(mod(n\\,%1)),format=pix_fmts=rgb24").arg(step)
			<< "-fps_mode" << "vfr";
	}
	else
	{
		arguments << "-filter:v" << "format=pix_fmts=rgb24";
	}

	// Per-format encoder option: -compression_algo is TIFF-only (deflate - lossless, and far better than the
	// encoder's packbits default on photographic frames); -qscale:v is the JPEG quality knob, meaningless for TIFF.
	if (tiff)
		arguments << "-compression_algo" << "deflate";
	else
		arguments << "-qscale:v" << QString::number(jpegQuality());
	arguments << QDir::toNativeSeparators(outputPattern);

	// Execute ffmpeg
	QProcess process;
	process.start(ffmpegPath(), arguments);

	if (!process.waitForStarted())
	{
		QMessageBox::critical(this, tr("Error"), tr("Failed to start FFMPEG process, check that it's present in PATH or configured in Settings.\nConfigured path: %1").arg(ffmpegPath()));
		cleanupAfterFailure();
		return;
	}

	if (!process.waitForFinished(300000)) // 5 minutes timeout
	{
		process.kill();
		QMessageBox::critical(this, tr("Error"), tr("FFMPEG process timeout (5 minutes)"));
		cleanupAfterFailure();
		return;
	}

	if (process.exitCode() != 0)
	{
		const QString errorOutput = process.readAllStandardError() + "\n" + process.readAllStandardOutput();
		QMessageBox::critical(this, tr("Error"),
			tr("FFMPEG failed with exit code %1\n\nError output:\n%2").arg(process.exitCode()).arg(errorOutput));
		cleanupAfterFailure();
		return;
	}

	// Check if frames were created
	const qsizetype frameCount = QDir(outputFolder).entryList({ "*.jpg", "*.tif" }, QDir::Files).count();
	if (frameCount == 0)
	{
		QMessageBox::warning(this, tr("Warning"), tr("No frames were extracted from:\n%1").arg(videoFilePath));
		cleanupAfterFailure();
		return;
	}

	// Register the video in the catalog now that extraction has actually produced frames - doing it only on
	// success avoids leaving "tracked" but empty/partial folders behind when ffmpeg fails partway through.
	// The source file is present here, so its MediaId is real; addMediaItem persists the source path + folder and
	// ensures the collection's folder label exists. It refuses if another item already owns this id (a
	// name+size collision) under a different folder - in that case don't leave the just-extracted frames
	// behind as an untracked duplicate.
	if (!Catalog::instance().addMediaItem(MediaId::fromFile(videoFilePath), videoFilePath, outputFolder, /*splitIntoFrames=*/true))
	{
		QMessageBox::critical(this, tr("Error"),
			tr("An item with the same name and file size is already tracked in a different collection:\n%1").arg(videoFilePath));
		cleanupAfterFailure();
	}
}

void MainWindow::processBatch(QStringList videoPaths, const QString& collectionPath, const QHash<MediaId, QString>& stagedPreviewDirs)
{
	if (videoPaths.empty())
		return;

	if (m_isProcessing)
	{
		QMessageBox::information(this, tr("Busy"), tr("Already extracting frames. Please wait for the current operation to finish."));
		return;
	}
	m_isProcessing = true;
	const auto processingGuard = qScopeGuard([this] { m_isProcessing = false; });

	// Each extracted video below registers via Catalog::addMediaItem (a couple of store writes); batch them into
	// one write for the whole call instead of one per video.
	Catalog::BatchScope batch;

	// Show processing message
	QMessageBox progressBox(this);
	progressBox.setWindowTitle(tr("Processing"));
	progressBox.setStandardButtons(QMessageBox::NoButton);
	progressBox.setModal(true);
	progressBox.show();

	bool conflictFound = false;
	// If a folder for the video already exists, ask about it later and process all unambiguous files first
	const auto partition = std::ranges::stable_partition(videoPaths, [&conflictFound, &collectionPath](const QString& path) {
		QFileInfo videoInfo(path);
		const QString outputFolder = collectionPath + "/" + videoInfo.completeBaseName();
		const bool conflict = QDir{ outputFolder }.exists();
		conflictFound |= conflict;
		return !conflict;
	});

	size_t i = 0;
	const auto processFilesRange = [&i, &progressBox, this, &collectionPath, &stagedPreviewDirs, totalSize = videoPaths.size()](const auto& begin, const auto& end, bool overwriteExisting = false) {
		for (const QString& videoPath : std::ranges::subrange(begin, end))
		{
			progressBox.setText(tr("Adding video %1/%2...").arg(++i).arg(totalSize));
			QApplication::processEvents();
			// Staged frames are keyed by the stable MediaId (re-derived here from the possibly-relocated path).
			const QString stagedPreviewDir = stagedPreviewDirs.value(MediaId::fromFile(videoPath));
			Import::Result result = Import::importVideo(videoPath, collectionPath, stagedPreviewDir, overwriteExisting);
			if (result.status == Import::Status::FolderConflict)
			{
				// Reached via the "decide one by one" batch choice below (or when two videos in one batch
				// share a base name, so the folder appeared after partitioning): ask about this one item,
				// then retry the call with overwrite granted.
				const QString outputFolder = collectionPath + "/" + QFileInfo(videoPath).completeBaseName();
				if (QMessageBox::question(this, tr("Folder Exists"),
						tr("Folder already exists:\n%1\n\nOverwrite?").arg(outputFolder),
						QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
					continue;
				result = Import::importVideo(videoPath, collectionPath, stagedPreviewDir, /*overwriteExisting=*/true);
			}
			if (result.status == Import::Status::Error)
				QMessageBox::critical(this, tr("Error"), result.errorMessage);
		}
	};

	// Process non-conflicting files first
	processFilesRange(videoPaths.begin(), partition.begin());

	if (partition.begin() != partition.end())
	{
		QMessageBox msgBox;
		msgBox.setIcon(QMessageBox::Question);
		msgBox.setWindowTitle(tr("Folder conflict"));
		msgBox.setText(tr("One or more videos have existing output folders. Overwrite all, skip all, or decide one by one?"));
		msgBox.setStandardButtons(QMessageBox::YesToAll | QMessageBox::Yes | QMessageBox::NoToAll);
		msgBox.button(QMessageBox::YesToAll)->setText(tr("Overwrite all"));
		msgBox.button(QMessageBox::Yes)->setText(tr("Decide one by one"));
		msgBox.button(QMessageBox::NoToAll)->setText(tr("Skip all"));
		msgBox.setDefaultButton(QMessageBox::YesToAll);

		const auto choice = msgBox.exec();
		if (choice != QMessageBox::NoToAll)
		{
			// Process conflicting files
			processFilesRange(partition.begin(), partition.end(), choice == QMessageBox::YesToAll);
		}
	}

	refreshLibraryView();
}

std::vector<Import::PhotoResult> MainWindow::processPhotoBatch(const QString& labelId, const QStringList& photoPaths, Import::PhotoImportMode mode)
{
	Catalog& catalog = Catalog::instance();
	const Catalog::Label* label = catalog.labelById(labelId);
	if (!label || label->isVirtual())
		return {};  // the label vanished from the catalog mid-session; the caller leaves everything staged

	Catalog::BatchScope batch;  // one store write for the whole batch instead of one per photo

	std::vector<Import::PhotoResult> results;
	results.reserve(photoPaths.size());
	for (const QString& path : photoPaths)
	{
		const Import::PhotoResult result = Import::importPhoto(path, label->displayName, mode);
		if (result.status == Import::PhotoStatus::Error)
			QMessageBox::critical(this, tr("Error"), result.errorMessage);
		// A referenced photo has no storage folder to derive its first label from - store it explicitly.
		// (An owned photo's label derives from the Photos/<label> dir its file just landed in.)
		if (result.status == Import::PhotoStatus::Success && mode == Import::PhotoImportMode::Reference)
			catalog.addLabel(result.registeredId, labelId);
		results.push_back(result);
	}

	refreshLibraryView();
	return results;
}

void MainWindow::reExportAllVideos()
{
	if (m_isProcessing)
	{
		QMessageBox::information(this, tr("Busy"), tr("Already extracting frames. Please wait for the current operation to finish."));
		return;
	}

	const auto confirm = QMessageBox::question(this, tr("Re-export all videos"),
		tr("This will delete and re-export all video frame folders where the source video is still available.\n\n"
		   "This applies to all collections, not just the current one.\n\nContinue?"),
		QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
	if (confirm != QMessageBox::Yes)
		return;

	// Collect all (videoPath, folderPath) pairs across all real collections
	struct VideoItem {
		QString videoPath;
		QString folderPath;
	};
	std::vector<VideoItem> toReExport;
	Catalog& catalog = Catalog::instance();
	for (const MediaId& id : catalog.allMediaItems())
	{
		if (catalog.mediaType(id) != Catalog::MediaType::Video)
			continue;  // a photo has no frames to re-export - and its "folder" is the shared Photos/<label> dir, which resplit would wipe

		const QString videoPath = catalog.sourcePathForMediaItem(id);
		if (!videoPath.isEmpty() && QFile::exists(videoPath))
			toReExport.push_back({ videoPath, catalog.folderForMediaItem(id) });
	}

	if (toReExport.empty())
	{
		QMessageBox::information(this, tr("Re-export all videos"), tr("No folders with an available source video were found."));
		return;
	}

	m_isProcessing = true;
	const auto processingGuard = qScopeGuard([this] { m_isProcessing = false; });

	// Each re-extracted video below re-registers via Catalog::addMediaItem; batch the whole pass into one write.
	Catalog::BatchScope batch;

	QMessageBox progressBox(this);
	progressBox.setWindowTitle(tr("Re-exporting"));
	progressBox.setStandardButtons(QMessageBox::NoButton);
	progressBox.setModal(true);
	progressBox.show();

	for (size_t i = 0, total = toReExport.size(); i < total; ++i)
	{
		progressBox.setText(tr("Re-exporting video %1/%2...").arg(i + 1).arg(total));
		QApplication::processEvents();

		const auto& [videoPath, folderPath] = toReExport[i];
		resplitVideoIntoFrames(videoPath, folderPath, /*preserveExistingPreview=*/false);
	}

	refreshMediaGrid();
}

bool MainWindow::createCollection(const QString& name, bool refreshList)
{
	// "Photos" is reserved for the owned-photo storage dir (<root>/Photos/<label>) - a collection folder by
	// that name would intermingle with it. Catalog::renameLabel refuses the same name.
	if (name.compare(BEST_COLLECTION_NAME, Qt::CaseInsensitive) == 0 || name.compare(PHOTOS_DIR_NAME, Qt::CaseInsensitive) == 0)
	{
		QMessageBox::warning(this, tr("Error"), tr("\"%1\" is a reserved name.").arg(name));
		return false;
	}

	if (!QDir{}.mkpath(rootFolder() + "/" + name))
	{
		QMessageBox::warning(this, tr("Error"), tr("Failed to create collection:\n%1").arg(rootFolder() + "/" + name));
		return false;
	}

	// Register the folder label now: an empty collection has no item to derive it from, so the catalog
	// won't surface it otherwise.
	Catalog::instance().createLabel(name);

	if (refreshList)
		refreshLibraryView();
	return true;
}

void MainWindow::createLabelInteractive()
{
	const QString name = QInputDialog::getText(this, tr("New label"), tr("Label name:")).trimmed();
	if (!name.isEmpty())
		createCollection(name);   // creates the backing folder; createCollection refreshes the view
}

void MainWindow::renameLabelInteractive(const QString& labelId)
{
	const Catalog::Label* label = Catalog::instance().labelById(labelId);
	if (!label)
		return;

	bool ok = false;
	const QString newName = QInputDialog::getText(this, tr("Rename label"), tr("New name:"), QLineEdit::Normal, label->displayName, &ok).trimmed();
	if (!ok || newName.isEmpty() || newName == label->displayName)
		return;

	if (!Catalog::instance().renameLabel(labelId, newName))
	{
		QMessageBox::warning(this, tr("Rename label"),
			tr("Could not rename to \"%1\". The name may already be in use, or a folder by that name already exists.").arg(newName));
		return;
	}
	refreshLibraryView();
}

void MainWindow::setLabelColorInteractive(const QString& labelId)
{
	const Catalog::Label* label = Catalog::instance().labelById(labelId);
	if (!label)
		return;

	const QColor initial = label->color.isEmpty() ? QColor(Qt::white) : QColor(label->color);
	const QColor chosen = QColorDialog::getColor(initial, this, tr("Label color"));
	if (!chosen.isValid())
		return;   // dialog cancelled

	Catalog::instance().setColor(labelId, chosen.name());   // "#rrggbb"
	refreshLibraryView();
}

void MainWindow::deleteLabelInteractive(const QString& labelId)
{
	Catalog& catalog = Catalog::instance();
	const Catalog::Label* label = catalog.labelById(labelId);
	if (!label)
		return;
	const QString name = label->displayName;

	const Catalog::DeleteImpact impact = catalog.deleteLabelImpact(labelId);
	if (impact.wouldOrphan)
	{
		QMessageBox::warning(this, tr("Delete label"),
			tr("Cannot delete \"%1\": some items are stored only under this label, with no other label to "
			   "fall back on. Give those items another label first, then delete this one.").arg(name));
		return;
	}

	QString message = tr("Delete the label \"%1\"?").arg(name);
	if (impact.relocateCount > 0)
		message += tr("\n\n%1 item(s) stored under it will be moved to another of their labels.").arg(impact.relocateCount);
	if (impact.untagCount > 0)
		message += tr("\n%1 item(s) tagged with it will lose the tag.").arg(impact.untagCount);
	message += tr("\n\nThis cannot be undone. Continue?");

	if (QMessageBox::warning(this, tr("Delete label"), message, QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
		return;

	if (!catalog.deleteLabel(labelId))
		QMessageBox::warning(this, tr("Delete label"), tr("Could not fully delete \"%1\" - some items may not have been moved.").arg(name));
	refreshLibraryView();
}

void MainWindow::quickImportToCollections(const QStringList& initialStaging)
{
	QuickImportDialog::Callbacks callbacks{
		// Every ordinary label is a candidate destination folder for the dialog's label list - single
		// source of truth (the Catalog model), not a disk listing, so the dialog also gets each label's color.
		.getLabelOptions = [] {
			std::vector<QuickImportDialog::LabelOption> options;
			for (const Catalog::Label& label : Catalog::instance().allLabels())
				if (!label.isVirtual())
					options.push_back(QuickImportDialog::LabelOption{ label.id, label.displayName, label.color });
			return options;
		},
		.addMediaItemsRequested = [this](const QString& collectionName, const QStringList& videoPaths,
				const QHash<MediaId, QString>& stagedPreviewDirs) {
			processBatch(videoPaths, rootFolder() + "/" + collectionName, stagedPreviewDirs);
		},
		.importPhotosRequested = [this](const QString& labelId, const QStringList& photoPaths, Import::PhotoImportMode mode) {
			return processPhotoBatch(labelId, photoPaths, mode);
		},
		.findAlreadyImportedDuplicatePhoto = [](const QString& photoPath) -> QString {
			// Byte-identical content already tracked as a photo, matched by size regardless of name - this is
			// what catches renamed duplicates. The size gate keeps the byte comparison rare.
			Catalog& catalog = Catalog::instance();
			const qint64 photoSize = QFileInfo(photoPath).size();
			for (const MediaId& id : catalog.allMediaItems())
			{
				if (catalog.mediaType(id) != Catalog::MediaType::Photo || id.size() != photoSize)
					continue;
				const QString existingPath = catalog.sourcePathForMediaItem(id);
				if (filesAreIdentical(photoPath, existingPath))
					return existingPath;
			}
			return {};
		},
		.createCollectionRequested = [this](const QString& name) -> bool {
			return createCollection(name, false);
		},
		.isMediaItemTrackedInCollection = [](const MediaId& id, const QString& collectionName) {
			// Compare against the exact folder this import derives (Import::importVideo's outputFolder): on a
			// name+size collision the id is tracked under some *other* folder - the staged copy was refused,
			// so a plain "tracked at all" check would misreport it as imported.
			const QString expectedFolder = rootFolder() + "/" + collectionName + "/" + QFileInfo(id.name()).completeBaseName();
			return QString::compare(Catalog::instance().folderForMediaItem(id), expectedFolder, Qt::CaseInsensitive) == 0;
		},
		.markBestRequested = [](const std::vector<MediaId>& bestItems) {
			// Items only reach this list after their import was confirmed, so "tracked in the catalog" is the
			// guard against a stray id - uniform across videos and photos (a photo has no per-item folder whose
			// existence could stand in for it).
			Catalog::BatchScope batch;  // one store write for the whole flush instead of one per item
			for (const MediaId& mediaId : bestItems)
				if (Catalog::instance().containsMediaItem(mediaId))
					Catalog::instance().addLabel(mediaId, Catalog::BestLabelId);
		},
		.assignExtraLabelsRequested = [](const std::vector<QuickImportDialog::ExtraLabelAssignment>& assignments) {
			// Mirrors markBestRequested above, same "tracked" guard.
			Catalog::BatchScope batch;  // one store write for the whole flush instead of one per assignment
			for (const QuickImportDialog::ExtraLabelAssignment& assignment : assignments)
			{
				if (!Catalog::instance().containsMediaItem(assignment.mediaId))
					continue;
				for (const QString& labelId : assignment.labelIds)
					Catalog::instance().addLabel(assignment.mediaId, labelId);
			}
		},
		.viewChanged = [this] { refreshLibraryView(); }
	};

	QuickImportDialog dialog(std::move(callbacks), Catalog::instance().anySourceDir(), this);
	if (!initialStaging.isEmpty())
		dialog.addToStaging(initialStaging);
	dialog.exec();   // each "Import" inside the dialog already applies its batch synchronously via the callbacks above

	// Catch-all refresh on close: each Import already repaints via the viewChanged callback, but creating a label
	// inline without then adding to it (createCollection(..., false) skips its own refresh) leaves the sidebar
	// stale otherwise.
	refreshLibraryView();
}

void MainWindow::scanForUntrackedFiles()
{
	const QStringList untrackedFiles = FindUntrackedFilesDialog::scanAndShowUi(rootFolder(), this);
	if (!untrackedFiles.isEmpty())
		quickImportToCollections(untrackedFiles);
}

void MainWindow::checkCatalogIntegrity()
{
	IntegrityCheckDialog::Callbacks callbacks{
		.relinkRequested = [this](const MediaId& placeholderId, const QString& sourcePath) {
			const bool ok = Catalog::instance().relinkPlaceholder(placeholderId, sourcePath);
			if (ok)
				refreshLibraryView();
			return ok;
		},
		.registerRequested = [this](const QString& folderPath, const QString& sourcePath) {
			const bool ok = Catalog::instance().addMediaItem(MediaId::fromFile(sourcePath), sourcePath, folderPath, /*splitIntoFrames=*/true);
			if (ok)
				refreshLibraryView();
			return ok;
		},
		.reimportRequested = [this](const MediaId& id) {
			// Same re-extraction path reExportAllVideos uses for any catalog video, just for this one: clear the
			// folder, re-run ffmpeg (which also regenerates the preview), and report whether frames actually landed.
			Catalog& catalog = Catalog::instance();
			const QString folder = catalog.folderForMediaItem(id);
			resplitVideoIntoFrames(catalog.sourcePathForMediaItem(id), folder, /*preserveExistingPreview=*/false);
			refreshLibraryView();
			return !QDir(folder).entryList(IMAGE_FILE_FILTERS, QDir::Files).isEmpty();
		},
		.regeneratePreviewRequested = [this](const MediaId& id) {
			const bool ok = regeneratePreviewFor(id);
			if (ok)
				refreshLibraryView();
			return ok;
		},
		.markSplitRequested = [this](const MediaId& id) {
			Catalog::instance().markSplitComplete(id);
			refreshLibraryView();
			return true;
		},
		.removeEntryRequested = [this](const MediaId& id) {
			Catalog::instance().removeMediaItem(id);
			refreshLibraryView();
			return true;
		},
	};
	IntegrityCheckDialog::scanAndShowUi(std::move(callbacks), this);
}

bool MainWindow::isInBest(const MediaId& id)
{
	return Catalog::instance().mediaItemHasLabel(id, Catalog::BestLabelId);
}

void MainWindow::toggleBestFolder(const MediaId& id)
{
	Catalog& catalog = Catalog::instance();
	if (catalog.mediaItemHasLabel(id, Catalog::BestLabelId))
		catalog.removeLabel(id, Catalog::BestLabelId);
	else
		catalog.addLabel(id, Catalog::BestLabelId);

	// The star's own checked state updates itself (it's a checkable QPushButton), but the dot strip needs
	// an explicit refresh - the resort/no-op branches below don't recreate the card, so without this the
	// dot would stay stale until the next full grid rebuild.
	for (int row = 0; row < m_mediaGrid->count(); ++row)
	{
		auto* item = static_cast<GridItem*>(m_mediaGrid->item(row));
		if (item->mediaId == id)
		{
			applyLabelDots(catalog, id, static_cast<MediaItemWidget*>(m_mediaGrid->itemWidget(item)));
			break;
		}
	}

	// If the Best filter is currently active, toggling Best changes which cards match - rebuild the grid;
	// otherwise just reposition under favorites-first (cheap reorder, no rebuild).
	if (m_labelSidebar->activeLabelIds().contains(Catalog::BestLabelId))
		QMetaObject::invokeMethod(this, &MainWindow::refreshMediaGrid, Qt::QueuedConnection);
	else if (m_sortControl->favoritesFirst())
		QMetaObject::invokeMethod(this, &MainWindow::resortMediaGrid, Qt::QueuedConnection);
}

void MainWindow::renameMediaItemInteractive(const MediaId& id)
{
	// Videos only - the menu entry is hidden for photos. The rename below derives video-shaped paths; on an
	// owned photo, folderForMediaItem is the SHARED Photos/<label> dir and would pass the exists-check.
	assert(Catalog::instance().mediaType(id) == Catalog::MediaType::Video);

	const QString originalFolderPath = Catalog::instance().folderForMediaItem(id);

	// The frame folder must exist
	if (originalFolderPath.isEmpty() || !QDir(originalFolderPath).exists())
	{
		QMessageBox::critical(this, tr("Rename media file"), tr("Frame folder does not exist:\n%1").arg(originalFolderPath));
		return;
	}

	const QString oldSourcePath = Catalog::instance().sourcePathForMediaItem(id);
	const bool sourceExists = !oldSourcePath.isEmpty() && QFile::exists(oldSourcePath);

	const QString oldName = QFileInfo(originalFolderPath).fileName();
	const QString parentPath = QFileInfo(originalFolderPath).absolutePath();

	// Ask for the new name
	const QString newName = QInputDialog::getText(this, tr("Rename media file"), tr("New name:"), QLineEdit::Normal, oldName).trimmed();

	if (newName.isEmpty() || newName == oldName)
		return;

	// Reject characters that are illegal in Windows file/folder names
	const QString invalidChars = R"(\/:*?"<>|)";
	for (const QChar c : newName)
	{
		if (invalidChars.contains(c))
		{
			QMessageBox::warning(this, tr("Rename media file"), tr("Name contains an invalid character: '%1'").arg(c));
			return;
		}
	}

	// Make sure the destination folder does not already exist
	const QString newFolderPath = parentPath + "/" + newName;
	if (QDir(newFolderPath).exists())
	{
		QMessageBox::warning(this, tr("Rename media file"), tr("A folder with that name already exists:\n%1").arg(newFolderPath));
		return;
	}

	// Build the new source path (same directory and extension, new base name)
	QString newSourcePath;
	if (!oldSourcePath.isEmpty())
	{
		const QFileInfo oldSourceInfo{ oldSourcePath };
		newSourcePath = oldSourceInfo.absolutePath() + "/" + newName + "." + oldSourceInfo.suffix();

		// Make sure we would not overwrite a different existing file
		if (sourceExists && QFile::exists(newSourcePath))
		{
			QMessageBox::warning(this, tr("Rename media file"), tr("A file with that name already exists:\n%1").arg(newSourcePath));
			return;
		}
	}

	// Build a confirmation message that spells out every change
	QString message = tr("Rename \u201c%1\u201d to \u201c%2\u201d?\n\n").arg(oldName, newName);
	if (sourceExists)
	{
		message += tr("\u2022 Source file:\n  %1\n  \u2192 %2\n\n").arg(oldSourcePath, newSourcePath);
	}
	else if (!oldSourcePath.isEmpty())
	{
		message += tr("\u2022 Source file not found at stored path \u2014 it will not be renamed.\n"
			"  The stored path will be updated to reflect the new name.\n\n");
	}
	message += tr("\u2022 Frame folder:\n  %1\n  \u2192 %2").arg(originalFolderPath, newFolderPath);

	if (isInBest(id))
		message += tr("\n\n\u2022 Best collection reference will be updated.");

	if (QMessageBox::question(this, tr("Rename media file"), message,
		QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes) != QMessageBox::Yes)
		return;

	renameMediaItem(id, newFolderPath);
}

void MainWindow::renameMediaItem(const MediaId& oldId, const QString& newFolderPath)
{
	const QString dialogTitle = tr("Rename media file");
	Catalog& catalog = Catalog::instance();
	const QString oldFolderPath = catalog.folderForMediaItem(oldId);
	const QString oldSourcePath = catalog.sourcePathForMediaItem(oldId);

	// --- Step 1: rename the source video file (if one is recorded and present) ---
	QString newSourcePath;
	bool sourceWasRenamed = false;
	MediaId newId = oldId;  // identity only changes if the source file itself is renamed (below)
	if (!oldSourcePath.isEmpty())
	{
		const QFileInfo oldSourceInfo{ oldSourcePath };
		newSourcePath = oldSourceInfo.absolutePath() + "/" + QFileInfo(newFolderPath).fileName() + "." + oldSourceInfo.suffix();

		if (QFile::exists(oldSourcePath))
		{
			if (!QFile::rename(oldSourcePath, newSourcePath))
			{
				QMessageBox::critical(this, dialogTitle, tr("Failed to rename the source file:\n%1\n\u2192 %2").arg(oldSourcePath, newSourcePath));
				return;
			}
			sourceWasRenamed = true;
			newId = MediaId::fromFile(newSourcePath);  // renaming the file changes its name and thus its MediaId
		}
	}

	// --- Step 2: rename the frame folder ---
	if (!QFile::rename(oldFolderPath, newFolderPath))
	{
		if (sourceWasRenamed)
			QFile::rename(newSourcePath, oldSourcePath);
		QMessageBox::critical(this, dialogTitle, tr("Failed to rename the frame folder:\n%1\n\u2192 %2").arg(oldFolderPath, newFolderPath));
		return;
	}

	// --- Step 3: update the catalog: carry the metadata record (loop intervals, labels incl. Best) to the new
	// identity and record the new source path + frame folder. Replaces the old re-key + source_info.txt rewrite.
	// Refused when the new name+size collides with another tracked item - undo the disk renames then, so disk
	// and catalog stay in sync (a collision requires a changed id, so the source file was renamed here too).
	if (!catalog.applyRename(oldId, newId, newSourcePath, newFolderPath))
	{
		QFile::rename(newFolderPath, oldFolderPath);
		if (sourceWasRenamed)
			QFile::rename(newSourcePath, oldSourcePath);
		QMessageBox::critical(this, dialogTitle,
			tr("An item with the same name and file size is already tracked in a different collection:\n%1").arg(newSourcePath));
		return;
	}

	// --- Step 4: refresh UI ---
	refreshLibraryView();
	if (m_frameViewer->currentFolder() == oldFolderPath && m_frameViewer->isVisible())
		m_frameViewer->showForFolder(newFolderPath);
}

void MainWindow::deleteFolderRecursively(const QString& folderPath)
{
	if (!QDir(folderPath).removeRecursively())
		QMessageBox::critical(this, tr("Error"), tr("Failed to delete folder: %1").arg(folderPath));
}
