#include "Windows/MainWindow.h"
#include "Core/Catalog.h"
#include "Windows/CompareWindow.h"
#include "Windows/PhotoCompareWindow.h"
#include "Ffmpeg.h"
#include "Import.h"
#include "Windows/FrameViewerWindow.h"
#include "Windows/IntegrityCheckDialog.h"
#include "UiComponents/LabelSidebar.h"
#include "UiComponents/LabelVisuals.h"
#include "Windows/ImportDialog.h"
#include "Windows/MediaRename.h"
#include "UiComponents/SegmentedToggle.h"
#include "Windows/SettingsDialog.h"
#include "UiComponents/SortControl.h"
#include "Core/MediaId.h"
#include "UiComponents/MediaGrid.h"
#include "UiComponents/MediaItemWidget.h"
#include "Windows/VideoPlayerWindow.h"
#include "Theme/Icons.h"
#include "Theme/Style.h"
#include "Theme/Theme.h"
#include "Utils.h"
#include "Settings.h"
#include "Shortcuts.h"

#include "aboutdialog/caboutdialog.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QColorDialog>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QDragEnterEvent>
#include <QFile>
#include <QFileDialog>
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
#include <QScrollBar>
#include <QSet>
#include <QSettings>
#include <QShortcut>
#include <QSplitter>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <QScopeGuard>

#include <algorithm>
#include <functional>
#include <utility>
#include <vector>

namespace {
// Strips the decoration from a combo popup's rows, so an icon set on the combo's items shows only on the
// closed control (which QComboBox paints from the current item) and isn't repeated down every dropdown row.
class ClosedControlIconDelegate final : public QStyledItemDelegate {
public:
	using QStyledItemDelegate::QStyledItemDelegate;
	void initStyleOption(QStyleOptionViewItem* option, const QModelIndex& index) const override
	{
		QStyledItemDelegate::initStyleOption(option, index);
		option->features &= ~QStyleOptionViewItem::HasDecoration;
		option->icon = QIcon();
	}
};
}

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
// Settings.h (Settings::PreviewFrameCount) since ImportDialog's staged cards mirror this choice too.
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
	setWindowTitle("Darkroom");
	resize(1500, 800);
	setAcceptDrops(true);

	m_frameViewer = new FrameViewerWindow();

	setupUI();

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
	saveSettings();
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
	m_nameFilter->addAction(Theme::tintedIcon(QStringLiteral(":/UI/icon_search.svg"), &Theme::ThemeColors::MutedText),
		QLineEdit::LeadingPosition);
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

	// Preview-frame-count selector. Self-labelling items ("N frames per preview") replace the usual separate QLabel + control pair.
	m_previewFrameCountCombo = new QComboBox();
	m_previewFrameCountCombo->setToolTip(tr("Number of preview frames shown on each video card"));
	const QIcon previewCountIcon = Theme::tintedIcon(QStringLiteral(":/UI/icon_columns.svg"), &Theme::ThemeColors::InstructionText);
	for (int n = MIN_PREVIEW_FRAME_COUNT; n <= MAX_PREVIEW_FRAME_COUNT; ++n)
		m_previewFrameCountCombo->addItem(previewCountIcon, (n == 1 ? tr("%1 frame per preview") : tr("%1 frames per preview")).arg(n), n);
	// The icon labels the control's purpose; show it only on the closed box, not repeated on every popup row.
	m_previewFrameCountCombo->view()->setItemDelegate(new ClosedControlIconDelegate(m_previewFrameCountCombo));

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
	m_mediaGrid = new MediaGrid();
	m_mediaGrid->setViewMode(QListView::IconMode);
	m_mediaGrid->setFlow(QListView::LeftToRight);
	m_mediaGrid->setWrapping(true);
	m_mediaGrid->setResizeMode(QListView::Adjust);
	// Cards come in two fixed sizes (a video's frame strip vs. a square photo), so setUniformItemSizes(true) -
	// which forces one size on every item - can't be used. Instead each GridItem carries an explicit sizeHint
	// (computed once per media type in refreshMediaGrid), so laying out an item stays a cached-value lookup
	// rather than a widget layout activation; populating hundreds of items is essentially as cheap as before.
	m_mediaGrid->setUniformItemSizes(false);
	m_mediaGrid->setMovement(QListView::Static);
	m_mediaGrid->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
	m_mediaGrid->setSelectionMode(QAbstractItemView::ExtendedSelection);
	// Dragging a card exports its source file(s): MediaGrid::startDrag turns the view's drag into a file:// URL
	// copy out to Explorer / another app. Enabling drags also makes the view keep an existing multi-selection
	// through a plain press+move instead of collapsing it to the single card under the cursor (it defers the
	// collapse while a drag is possible), so a whole group can be dragged out together.
	m_mediaGrid->setDragEnabled(true);
	m_mediaGrid->setDragUrlsProvider([this](const QList<QListWidgetItem*>& items) { return dragUrlsForItems(items); });
	m_mediaGrid->setSpacing(10);
	Style::applyThemedSheet(m_mediaGrid, [] {
		return QStringLiteral("QListWidget::item:selected { background-color: %1; }").arg(Theme::current().AccentBg);
	});

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

	QMenu* editMenu = new QMenu(tr("Edit"), menuBar);
	m_deleteAction = editMenu->addAction(tr("Delete"), QKeySequence(Shortcuts::DeleteFile), this, &MainWindow::deleteSelectedItems);
	m_removeFromLibraryAction = editMenu->addAction(tr("Remove from library"), QKeySequence(Shortcuts::RemoveFromList), this, &MainWindow::removeSelectedItemsFromLibrary);
	m_renameAction = editMenu->addAction(tr("Rename"), QKeySequence(Shortcuts::Rename), this, &MainWindow::renameSelectedItemInteractive);
	connect(m_mediaGrid, &QListWidget::itemSelectionChanged, this, &MainWindow::updateEditActions);
	updateEditActions();

	QMenu* toolsMenu = new QMenu(tr("Tools"), menuBar);
	toolsMenu->addAction(tr("Import to collections..."), QKeySequence("Ctrl+Shift+A"), this, [this] { importToCollections(); });
	toolsMenu->addAction(tr("Scan for untracked files..."), this, &MainWindow::scanForUntrackedFiles);
	toolsMenu->addAction(tr("Check catalog integrity..."), this, &MainWindow::checkCatalogIntegrity);
	toolsMenu->addSeparator();
	toolsMenu->addAction(tr("Compare photos..."), QKeySequence("Shift+C"), this, [this] {
		auto* w = new PhotoCompareWindow({}, this);  // opens empty; photos are dropped in
		w->setAttribute(Qt::WA_DeleteOnClose);
		w->show();
	});
	toolsMenu->addSeparator();
	toolsMenu->addAction(tr("Restart all videos"), QKeySequence("Shift+R"), this, &VideoPlayerWindow::restartAll);
	toolsMenu->addAction(tr("Close all videos"),   QKeySequence("Shift+W"), this, &VideoPlayerWindow::closeAll);
	toolsMenu->addSeparator();
	toolsMenu->addAction(tr("Re-export all videos"), QKeySequence("Ctrl+Shift+E"), this, &MainWindow::reExportAllVideos);

	QMenu* helpMenu = new QMenu(tr("Help"), menuBar);
	helpMenu->addAction(tr("About Darkroom..."), this, [this] {
		CAboutDialog(QApplication::applicationVersion(), this).exec();
	});

	menuBar->addMenu(fileMenu);
	menuBar->addMenu(editMenu);
	menuBar->addMenu(toolsMenu);
	menuBar->addMenu(helpMenu);

	for (QAction* action : menuBar->actions())
	{
		if (!action->menu())
			continue;
		for (QAction* sub : action->menu()->actions())
			sub->setShortcutContext(Qt::ApplicationShortcut);
	}

	// Del/Shift+Del/F2 must not fire while typing in a text field - scope them to the grid.
	for (QAction* a : { m_deleteAction, m_removeFromLibraryAction, m_renameAction })
	{
		m_mediaGrid->addAction(a);
		a->setShortcutContext(Qt::WidgetWithChildrenShortcut);
	}
}

void MainWindow::saveSettings()
{
	saveWindowGeometry(this, "mainWindow");
	QVariantList activeIds;  // stored as native quint64s (a LabelId is not a QVariant-native type)
	for (const LabelId id : m_labelSidebar->activeLabelIds())
		activeIds << static_cast<qulonglong>(toUInt64(id));
	QSettings{}.setValue("mainWindow/activeLabelIds", activeIds);
	QSettings{}.setValue("mainWindow/labelsAndMode", m_labelSidebar->isAndMode());
}

void MainWindow::restoreSettings()
{
	restoreWindowGeometry(this, "mainWindow");

	QList<LabelId> activeIds;
	for (const QVariant& v : QSettings{}.value("mainWindow/activeLabelIds").toList())
		activeIds << labelIdFromUInt64(v.toULongLong());  // an unparseable stored value reads back as None and is dropped on refresh
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
	// A card dragged out of the grid carries its source file:// URL(s) for export to other apps. Those URLs are
	// "supported files", so without this the drop would land back on our own import handler and re-import an
	// already-tracked item. The import drop zone is for files coming from outside the app.
	if (event->source() == m_mediaGrid)
		return;

	if (hasSupportedPaths(event->mimeData()))
		event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* event)
{
	// Files and folders are forwarded as-is; the Import dialog expands any folder into the supported files under it.
	QStringList paths = supportedPaths(event->mimeData());

	// There is no "current collection" in the label model, so hand the dropped paths to the Import dialog, where the
	// user picks the destination label(s). Deferred so the drop source application is released promptly
	// instead of being held in the drag state until processing finishes.
	QMetaObject::invokeMethod(this, [this, paths = std::move(paths)] {
		importToCollections(paths);
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
// shows Best so the card's label display is uniform across all labels. Also composes the card's single
// tooltip (owned by the thumbnail): for a video, its frame-extraction state on the first line, then the labels.
void applyLabelDots(Catalog& catalog, const MediaId& id, MediaItemWidget* card)
{
	std::vector<QColor> dotColors;
	QStringList dotNames;
	for (const LabelId labelId : catalog.labelsForMediaItem(id))
	{
		const Catalog::Label* label = catalog.labelById(labelId);
		if (!label)
			continue;
		dotColors.push_back(label->color.isEmpty() ? QColor() : QColor(label->color));
		dotNames << label->displayName;
	}

	QString stateLine;
	if (catalog.mediaType(id) == Catalog::MediaType::Video)  // photos have no frames to extract, so no state line
		stateLine = (catalog.isSplitIntoFrames(id) ? MainWindow::tr("Frames extracted")
		                                           : MainWindow::tr("Not extracted yet - middle-click to extract"))
		            + QLatin1String("\n");
	card->setLabelDots(dotColors, stateLine + MainWindow::tr("Labels: %1").arg(dotNames.join(", ")));
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

	const Ffmpeg::PreviewResult result = Ffmpeg::generatePreviewFrames(source, Catalog::previewDirFor(folder), frameCount);
	catalog.setDurationMs(id, result.durationMs);  // backfill: an item imported before durations were recorded gets one here (no-op if the probe failed)
	return result.ok();
}

std::vector<MediaId> MainWindow::mediaItemsMatchingFilters() const
{
	Catalog& catalog = Catalog::instance();

	// The sidebar's active label filter applied to the catalog.
	const QList<LabelId> activeLabelIds = m_labelSidebar->activeLabelIds();
	std::vector<MediaId> mediaItems;
	if (activeLabelIds.isEmpty())
	{
		const auto& all = catalog.mediaItems();  // no filter ("All"): the whole catalog
		mediaItems.assign(all.keyBegin(), all.keyEnd());
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

	return mediaItems;
}

MediaItemWidget* MainWindow::buildMediaCard(const MediaId& id, bool isBest, const QSize& photoCanvas, const QSize& videoCanvas, int previewFrameCount)
{
	Catalog& catalog = Catalog::instance();
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
			return nullptr;  // preview/ missing or empty (externally deleted folder, or preview generation failed outright) - don't show a frameless ghost card

		previewPaths = pickEvenlySpacedFrames(previewDir, imageFiles, previewFrameCount);
	}

	auto* card = new MediaItemWidget(
		isPhoto ? photoCanvas : videoCanvas,
		previewPaths, QString(),
		id,
		isBest,
		[this, id] { toggleBest(id); },
		// double-click: a video opens in the built-in player, a photo in the system image viewer
		[this, id, isPhoto] { if (isPhoto) openSourceInSystemApp(id); else playVideo(id); },
		[this, id](QPoint globalPos) { showMediaItemContextMenu(id, globalPos); },
		/* dynamic size hint */false,
		/* film strip */ !isPhoto   // videos read as a perforated film strip; photos stay a plain square
	);
	if (!isPhoto)  // the frame viewer browses a video's frame folder; a photo has no frames, so no middle-click
	{
		card->setOnMiddleButtonClick([this, id, folderPath] {
			ensureFramesSplit(id);  // transparently runs the full split first, if this video hasn't had one yet
			m_frameViewer->showForFolder(folderPath);
		});
	}
	card->setOnMouseWheelCallback([this](int steps) { zoomCards(steps); });
	card->setFramesExtracted(!isPhoto && catalog.isSplitIntoFrames(id));  // green "frames ready" badge - videos only (a photo reports split==true but has no frames)
	card->setDuration(catalog.durationMsForMediaItem(id));  // shows the duration pill on videos; photos / not-yet-probed videos return -1 -> no pill

	// A label dragged from the sidebar onto this card is added to it (or to the whole selection if this card
	// is part of it). Mirrors the context-menu "Labels" add path; drag only ever adds, never removes.
	// Deferred: refreshLibraryView rebuilds the grid, deleting this very card mid-dropEvent, so the mutation
	// runs after the drop event unwinds (same reason MainWindow::dropEvent defers opening the Import dialog).
	card->setOnLabelDropped([this, id](const QString& labelId) {
		const std::vector<MediaId> targets = effectiveSelection(id);
		const LabelId dropped = labelIdFromString(labelId);  // the mime payload is the id's decimal string
		QMetaObject::invokeMethod(this, [this, targets, dropped] {
			Catalog::BatchScope batch;  // one store write for the whole selection instead of one per item
			for (const MediaId& target : targets)
				Catalog::instance().addLabel(target, dropped);
			refreshLibraryView();
		}, Qt::QueuedConnection);
	});

	applyLabelDots(catalog, id, card);
	return card;
}

void MainWindow::refreshMediaGrid()
{
	m_mediaGrid->clear();

	const int previewFrameCount = m_previewFrameCountCombo->currentData().toInt();
	const int imageHeight = cardImageHeight();

	// Card thumbnail canvases: a photo is square; a video is a horizontal strip sized to tile with
	// previewFrameCount photo cards (so a video's width + gap spans that many photo widths + gaps), letting a
	// mixed grid line up on one column grid. Both are fixed per refresh, so each is computed once here.
	const QSize photoCanvas{ imageHeight, imageHeight };
	const QSize videoCanvas{ MediaItemWidget::videoCanvasWidthForTiling(imageHeight, previewFrameCount, m_mediaGrid->spacing()), imageHeight };

	// The catalog is the authoritative in-memory model, kept current by its mutations, so the grid reads it
	// directly - no per-refresh re-derivation. Look up the Best set once: the per-card star state and the
	// favorites-first sort below share it.
	Catalog& catalog = Catalog::instance();
	const QSet<MediaId> bestSet = catalog.mediaItemsForLabel(Catalog::BestLabelId);

	GridItem::favoritesFirst = m_sortControl->favoritesFirst();
	GridItem::sortBy = m_sortControl->sortBy();
	GridItem::descending = m_sortControl->descending();
	const bool sortByDate = GridItem::sortBy == SortBy::Date;

	// Media items to show = the structural filters applied to the catalog (final order comes from sortItems()
	// below, so the enumeration order doesn't matter).
	const std::vector<MediaId> mediaItems = mediaItemsMatchingFilters();

	// Empty-state text, painted by the grid when no card is visible. "Library empty" is the catalog's own
	// state, not something inferred from this rebuild's filters; every other empty case (the label/type
	// filters, or the name filter hiding every card later) is the filters matching nothing.
	m_mediaGrid->setEmptyMessage(catalog.mediaItemCount() == 0
		? tr("The library is empty.\nDrop media files here, or use Tools > Import to collections.")
		: tr("No items match the current filters."));

	// Card widgets are attached in a second pass (after the addItem loop and sortItems), not inline: interleaving
	// setItemWidget with addItem makes each insert's endInsertRows walk every persistent editor index created so
	// far -> O(N^2). This list carries each item and its not-yet-attached card between the two passes.
	std::vector<std::pair<GridItem*, MediaItemWidget*>> pendingAttach;
	pendingAttach.reserve(mediaItems.size());

	// Cards come in two fixed sizes, one per media type (a video's frame strip, a photo's square), so the size
	// hint for each type is computed once from that type's first built card and reused - a card's size depends
	// only on its type. Querying every card would needlessly activate each one's layout. The two widths are
	// chosen to tile on one column grid (see photoCanvas/videoCanvas above), so a mixed grid still lines up.
	QSize videoCardHint, photoCardHint;

	// Pass 1: build cards + insert bare items (editor-free, so O(N)), in enumeration order with their sort
	// info attached; sortItems() below (via GridItem::operator<) puts them in their final order, and captions
	// are numbered after that.
	for (const MediaId& id : mediaItems)
	{
		const bool isBest = bestSet.contains(id);
		MediaItemWidget* card = buildMediaCard(id, isBest, photoCanvas, videoCanvas, previewFrameCount);
		if (!card)
			continue;  // a video with no preview frames gets no card (see buildMediaCard)

		auto* item = new GridItem();
		item->mediaId = id;
		item->info = itemInfoFor(catalog, id, isBest, sortByDate);
		QSize& typeHint = catalog.mediaType(id) == Catalog::MediaType::Photo ? photoCardHint : videoCardHint;   // one cached hint per media type (see above)
		if (!typeHint.isValid())
			typeHint = card->sizeHint();
		item->setSizeHint(typeHint);
		m_mediaGrid->addItem(item);

		pendingAttach.emplace_back(item, card);   // widget attached in the second pass, after sortItems()
	}

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

std::vector<MediaId> MainWindow::effectiveSelection(std::optional<MediaId> id) const
{
	std::vector<MediaId> selected;
	for (const QListWidgetItem* item : m_mediaGrid->selectedItems())
		selected.push_back(static_cast<const GridItem*>(item)->mediaId);
	if (id && std::find(selected.cbegin(), selected.cend(), *id) == selected.cend())
		selected = { *id };
	return selected;
}

QList<QUrl> MainWindow::dragUrlsForItems(const QList<QListWidgetItem*>& items) const
{
	Catalog& catalog = Catalog::instance();
	QList<QUrl> urls;
	urls.reserve(items.size());
	for (const QListWidgetItem* item : items)
	{
		const QString path = catalog.sourcePathForMediaItem(static_cast<const GridItem*>(item)->mediaId);
		if (!path.isEmpty() && QFile::exists(path))
			urls << QUrl::fromLocalFile(path);
	}
	return urls;
}

QString MainWindow::bulletedItemNameList(const std::vector<MediaId>& selection)
{
	const Catalog& catalog = Catalog::instance();
	QString list;
	constexpr size_t maxListed = 15;
	for (size_t i = 0; i < std::min(maxListed, selection.size()); ++i)
	{
		const MediaId& sel = selection[i];
		// A video is named by its frame folder; a photo's folder is shared/empty, so use the id's file name.
		list += "\n• " + (catalog.mediaType(sel) == Catalog::MediaType::Photo
			? sel.name() : QFileInfo(catalog.folderForMediaItem(sel)).fileName());
	}
	if (selection.size() > maxListed)
		list += "\n" + tr("... and %1 more").arg(selection.size() - maxListed);
	return list;
}

void MainWindow::deleteSelectedItems()
{
	const std::vector<MediaId> selection = effectiveSelection(m_contextMenuTarget);
	if (selection.empty())
		return;

	Catalog& catalog = Catalog::instance();

	// What "delete" means per type: a video loses its frame folder and source file; a photo loses its
	// file ONLY - its folderForMediaItem is the shared Photos/<label> dir, home to sibling photos,
	// and must never be deleted.

	// The confirmation lists what goes. A single item: its exact paths. A multi-selection: the count plus
	// the item names (capped, so a huge selection stays readable).
	QString message;
	if (selection.size() == 1)
	{
		const MediaId& sel = selection.front();
		const QString sourcePath = catalog.sourcePathForMediaItem(sel);
		if (catalog.mediaType(sel) == Catalog::MediaType::Photo)
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
		bool anyVideo = false, anyPhoto = false;
		for (const MediaId& sel : selection)
		{
			if (catalog.mediaType(sel) == Catalog::MediaType::Video)
				anyVideo = true;
			else
				anyPhoto = true;
		}

		QStringList deletedKinds;
		if (anyVideo)
			deletedKinds << tr("each video's frame folder and source file");
		if (anyPhoto)
			deletedKinds << tr("each photo's file");
		message = tr("This will permanently delete %1 items - %2:\n").arg(selection.size()).arg(deletedKinds.join(", "));

		message += bulletedItemNameList(selection);
	}
	message += tr("\n\nThis cannot be undone. Continue?");

	if (QMessageBox::warning(this, tr("Delete"), message,
		QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
		return;

	Catalog::BatchScope batch;  // one store write for the whole selection instead of one per item
	for (const MediaId& sel : selection)
	{
		const QString sourcePath = catalog.sourcePathForMediaItem(sel);
		if (catalog.mediaType(sel) == Catalog::MediaType::Photo)
		{
			QFile::remove(sourcePath);
			catalog.removeMediaItem(sel);
			continue;
		}

		const QString folderPath = catalog.folderForMediaItem(sel);
		deleteFolderRecursively(folderPath);
		if (!sourcePath.isEmpty())
			QFile::remove(sourcePath);
		catalog.removeMediaItem(sel);

		if (m_frameViewer->currentFolder() == folderPath)
			m_frameViewer->showForFolder({});
	}

	refreshLibraryView();
}

void MainWindow::removeSelectedItemsFromLibrary()
{
	const std::vector<MediaId> selection = effectiveSelection(m_contextMenuTarget);
	if (selection.empty())
		return;

	Catalog& catalog = Catalog::instance();

	// Untrack drops the catalog entry only - nothing on disk is touched. The catalog is never re-derived
	// from a disk walk, so an untracked video's frame folder stays out of the library until the integrity
	// tool surfaces it as untracked (or the source video is re-imported).
	QString message;
	if (selection.size() == 1)
	{
		const MediaId& sel = selection.front();
		message = tr("This will remove the item from the library:\n");
		if (catalog.mediaType(sel) == Catalog::MediaType::Video)
			message += "\n• " + catalog.folderForMediaItem(sel);
		const QString sourcePath = catalog.sourcePathForMediaItem(sel);
		if (!sourcePath.isEmpty())
			message += "\n• " + sourcePath;
	}
	else
	{
		message = tr("This will remove %1 items from the library:\n").arg(selection.size());
		message += bulletedItemNameList(selection);
	}
	message += "\n\n" + tr("No files will be deleted, but labels and other catalog metadata will be discarded. Continue?");

	if (QMessageBox::question(this, tr("Remove from library"), message,
		QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
		return;

	Catalog::BatchScope batch;  // one store write for the whole selection instead of one per item
	for (const MediaId& sel : selection)
		catalog.removeMediaItem(sel);
	// The frame viewer is deliberately left open if it's showing one of these folders - the frames stay on disk.

	refreshLibraryView();
}

void MainWindow::renameSelectedItemInteractive()
{
	const auto selected = m_mediaGrid->selectedItems();
	if (selected.size() != 1)
		return;
	renameItemInteractive(static_cast<const GridItem*>(selected.front())->mediaId);
}

void MainWindow::updateEditActions()
{
	const auto selected = m_mediaGrid->selectedItems();
	const bool hasSelection = !selected.empty();
	m_deleteAction->setEnabled(hasSelection);
	m_removeFromLibraryAction->setEnabled(hasSelection);

	// Both videos and photos can be renamed, so the action only needs exactly one selected item.
	m_renameAction->setEnabled(selected.size() == 1);
}

void MainWindow::showMediaItemContextMenu(const MediaId& id, const QPoint& globalPos)
{
	m_contextMenuTarget = id;
	Catalog& catalog = Catalog::instance();
	const std::vector<MediaId> selection = effectiveSelection(id);
	const QString folderPath = catalog.folderForMediaItem(id);
	const bool isPhoto = catalog.mediaType(id) == Catalog::MediaType::Photo;

	QMenu menu(this);

	// One pass classifies the selection for the two compare actions below; each is offered only for a
	// selection homogeneous in its media type.
	size_t videoCount = 0;
	for (const MediaId& sel : selection)
		if (catalog.mediaType(sel) == Catalog::MediaType::Video)
			++videoCount;
	const bool selectionAllVideos = videoCount == selection.size();
	const bool selectionAllPhotos = videoCount == 0;

	// CompareWindow browses frame folders, so it's videos-only (photos get their own PhotoCompareWindow below).
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
	if (selectionAllPhotos && selection.size() >= 2)
	{
		menu.addAction(tr("Compare photos"), [this, selection] {
			QStringList paths;
			for (const MediaId& sel : selection)
				paths << Catalog::instance().sourcePathForMediaItem(sel);
			PhotoCompareWindow::showForFiles(paths, this);
		});
		menu.addSeparator();
	}

	// A photo's folderForMediaItem is the shared Photos/<label> dir (or nothing when referenced), not a
	// folder of its own to open - "Locate source file" below is how to reach the photo itself.
	if (!isPhoto)
	{
		menu.addAction(tr("Open in Explorer"), [folderPath, this] {
			if (!openInExplorer(folderPath))
				reportMissingFile(this, folderPath);
		});
	}
	menu.addAction(isPhoto ? tr("Open photo") : tr("Play source video"), [this, id] {
		openSourceInSystemApp(id);
	});
	menu.addAction(tr("Locate source file"), [this, id] {
		const QString sourcePath = Catalog::instance().sourcePathForMediaItem(id);
		if (sourcePath.isEmpty())
			QMessageBox::warning(this, tr("Error"), tr("No source file is recorded for this item."));
		else if (!openInExplorer(sourcePath))
			reportMissingFile(this, sourcePath);
	});
	menu.addAction(tr("Copy source path to clipboard"), [id] {
		const QString sourcePath = Catalog::instance().sourcePathForMediaItem(id);
		if (!sourcePath.isEmpty())
			QApplication::clipboard()->setText(QDir::toNativeSeparators(sourcePath));
	});
	auto* renameEntry = menu.addAction(isPhoto ? tr("Rename photo") : tr("Rename media file"), [this, id] {
		renameItemInteractive(id);
	});
	renameEntry->setShortcut(m_renameAction->shortcut());
	renameEntry->setShortcutContext(Qt::WidgetShortcut);
	menu.addSeparator();

	const bool inBest = isInBest(id);
	menu.addAction(inBest ? tr("Remove from Best") : tr("Add to Best"), [this, id] {
		toggleBest(id);
	});

	// Labels submenu: a checklist of every ordinary label. Each row's color-tinted checkbox reflects the whole
	// effective selection - checked when every selected item has the label, a dash when only some do, an empty box
	// when none do. Toggling makes the selection uniform: strip the label when all already carry it, else add to all.
	std::vector<LabelVisuals::ChecklistRow> labelRows;
	for (const Catalog::Label& label : catalog.allLabels())
	{
		if (label.isVirtual())  // Best is the star / "Add to Best" action above, not a dot/checkbox
			continue;
		const LabelId labelId = label.id;

		int haveCount = 0;
		for (const MediaId& sel : selection)
			if (catalog.mediaItemHasLabel(sel, labelId))
				++haveCount;

		labelRows.push_back({ label.displayName, QColor(label.color),
			LabelVisuals::presenceForCount(haveCount, static_cast<int>(selection.size())),
			[this, selection, labelId](bool addToAll) {
				Catalog::BatchScope batch;  // one store write for the whole selection instead of one per item
				for (const MediaId& target : selection)
				{
					if (addToAll)
						Catalog::instance().addLabel(target, labelId);
					else
						Catalog::instance().removeLabel(target, labelId);
				}
				refreshLibraryView();
			} });
	}
	LabelVisuals::buildChecklistMenu(menu.addMenu(tr("Labels")), std::move(labelRows));
	menu.addSeparator();

	{
		auto* a = menu.addAction(selection.size() > 1 ? tr("Remove %1 items from library (untrack)").arg(selection.size()) : tr("Remove from library (untrack)"),
			this, &MainWindow::removeSelectedItemsFromLibrary);
		a->setShortcut(m_removeFromLibraryAction->shortcut());
		a->setShortcutContext(Qt::WidgetShortcut);
	}
	{
		auto* a = menu.addAction(selection.size() > 1 ? tr("Delete (%1 items)").arg(selection.size()) : tr("Delete"),
			this, &MainWindow::deleteSelectedItems);
		a->setShortcut(m_deleteAction->shortcut());
		a->setShortcutContext(Qt::WidgetShortcut);
	}

	menu.exec(globalPos);
	m_contextMenuTarget = std::nullopt;
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
		const Ffmpeg::PreviewResult result = Ffmpeg::generatePreviewFrames(videoFilePath, Catalog::previewDirFor(outputFolder), m_previewFrameCountCombo->currentData().toInt());
		// splitVideoIntoFrames just (re-)registered this item; record the duration its probe produced, so a
		// re-export of a video imported before durations were recorded backfills one (no-op if the probe failed).
		Catalog::instance().setDurationMs(MediaId::fromFile(videoFilePath), result.durationMs);
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
	// The ffmpeg invocation itself lives in the Ffmpeg module; this wrapper only supplies the settings-derived
	// options, renders the outcome, and - on success - registers the video in the catalog.
	const Ffmpeg::SplitOptions options{ .tiff = useTiff(), .jpegQuality = jpegQuality(), .frameStep = frameStep() };
	const Ffmpeg::SplitResult result = Ffmpeg::splitVideoIntoFrames(videoFilePath, outputFolder, options);

	using Status = Ffmpeg::SplitResult::Status;
	switch (result.status)
	{
	case Status::Ok:
		break;
	case Status::SourceMissing:
		reportMissingFile(this, videoFilePath);
		return;
	case Status::FolderCreateFailed:
		QMessageBox::critical(this, tr("Error"), tr("Failed to create output folder:\n%1").arg(outputFolder));
		return;
	case Status::StartFailed:
		QMessageBox::critical(this, tr("Error"), tr("Failed to start FFMPEG process, check that it's present in PATH or configured in Settings.\nConfigured path: %1").arg(ffmpegPath()));
		return;
	case Status::TimedOut:
		QMessageBox::critical(this, tr("Error"), tr("FFMPEG process timeout (5 minutes)"));
		return;
	case Status::ExtractionFailed:
		QMessageBox::critical(this, tr("Error"),
			tr("FFMPEG failed with exit code %1\n\nError output:\n%2").arg(result.exitCode).arg(result.errorOutput));
		return;
	case Status::NoFrames:
		QMessageBox::warning(this, tr("Warning"), tr("No frames were extracted from:\n%1").arg(videoFilePath));
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
		QDir(outputFolder).removeRecursively();
	}
}

void MainWindow::importVideoBatch(QStringList videoPaths, const QString& collectionPath, const QHash<MediaId, QString>& stagedPreviewDirs, const QHash<MediaId, qint64>& stagedDurations)
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

	// If a folder for the video already exists, ask about it later and process all unambiguous files first
	const auto partition = std::ranges::stable_partition(videoPaths, [&collectionPath](const QString& path) {
		const QString outputFolder = collectionPath + "/" + QFileInfo(path).completeBaseName();
		return !QDir{ outputFolder }.exists();
	});

	// firstNumber: the 1-based display index of the range's first video, so the conflicting range (the second
	// call below) continues the progress numbering where the unambiguous one stopped.
	const auto processFilesRange = [&progressBox, this, &collectionPath, &stagedPreviewDirs, &stagedDurations, totalSize = videoPaths.size()](const auto& begin, const auto& end, qsizetype firstNumber, bool overwriteExisting = false) {
		qsizetype displayNumber = firstNumber;
		for (const QString& videoPath : std::ranges::subrange(begin, end))
		{
			progressBox.setText(tr("Adding video %1/%2...").arg(displayNumber++).arg(totalSize));
			QApplication::processEvents();
			// Staged frames and the staged duration are keyed by the stable MediaId (re-derived here from the possibly-relocated path).
			const MediaId id = MediaId::fromFile(videoPath);
			const QString stagedPreviewDir = stagedPreviewDirs.value(id);
			const qint64 stagedDurationMs = stagedDurations.value(id, -1);
			Import::Result result = Import::importVideo(videoPath, collectionPath, stagedPreviewDir, overwriteExisting, stagedDurationMs);
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
				result = Import::importVideo(videoPath, collectionPath, stagedPreviewDir, /*overwriteExisting=*/true, stagedDurationMs);
			}
			if (result.status == Import::Status::Error)
				QMessageBox::critical(this, tr("Error"), result.errorMessage);
		}
	};

	// Process non-conflicting files first
	processFilesRange(videoPaths.begin(), partition.begin(), 1);

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
			processFilesRange(partition.begin(), partition.end(), partition.begin() - videoPaths.begin() + 1, choice == QMessageBox::YesToAll);
		}
	}

	refreshLibraryView();
}

std::vector<Import::PhotoResult> MainWindow::importPhotoBatch(LabelId labelId, const QStringList& photoPaths, Import::PhotoImportMode mode)
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
	for (const Catalog::Entry& entry : catalog.mediaItems())
	{
		if (entry.type != Catalog::MediaType::Video)
			continue;  // a photo has no frames to re-export - and its "folder" is the shared Photos/<label> dir, which resplit would wipe

		if (!entry.sourcePath.isEmpty() && QFile::exists(entry.sourcePath))
			toReExport.push_back({ entry.sourcePath, entry.folder });
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

LabelId MainWindow::createCollection(const QString& name, const QString& color, bool refreshList)
{
	// "Photos" is reserved for the owned-photo storage dir (<root>/Photos/<label>) - a collection folder by
	// that name would intermingle with it. Catalog::renameLabel refuses the same name.
	if (name.compare(BEST_COLLECTION_NAME, Qt::CaseInsensitive) == 0 || name.compare(PHOTOS_DIR_NAME, Qt::CaseInsensitive) == 0)
	{
		QMessageBox::warning(this, tr("Error"), tr("\"%1\" is a reserved name.").arg(name));
		return {};
	}

	if (!QDir{}.mkpath(rootFolder() + "/" + name))
	{
		QMessageBox::warning(this, tr("Error"), tr("Failed to create collection:\n%1").arg(rootFolder() + "/" + name));
		return {};
	}

	// Register the folder label now: an empty collection has no item to derive it from, so the catalog
	// won't surface it otherwise.
	const LabelId labelId = Catalog::instance().createLabel(name, color);

	if (refreshList)
		refreshLibraryView();
	return labelId;
}

void MainWindow::createLabelInteractive()
{
	const QString name = QInputDialog::getText(this, tr("New label"), tr("Label name:")).trimmed();
	if (!name.isEmpty())
		createCollection(name);   // creates the backing folder; createCollection refreshes the view
}

void MainWindow::renameLabelInteractive(LabelId labelId)
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

void MainWindow::setLabelColorInteractive(LabelId labelId)
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

void MainWindow::deleteLabelInteractive(LabelId labelId)
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

void MainWindow::importToCollections(const QStringList& initialStaging)
{
	ImportDialog::Callbacks callbacks{
		.addMediaItemsRequested = [this](const QString& collectionName, const QStringList& videoPaths,
				const QHash<MediaId, QString>& stagedPreviewDirs, const QHash<MediaId, qint64>& stagedDurations) {
			importVideoBatch(videoPaths, rootFolder() + "/" + collectionName, stagedPreviewDirs, stagedDurations);
		},
		.importPhotosRequested = [this](const QString& labelId, const QStringList& photoPaths, Import::PhotoImportMode mode) {
			return importPhotoBatch(labelIdFromString(labelId), photoPaths, mode);
		},
		.createCollectionRequested = [this](const QString& name, const QString& color) -> QString {
			const LabelId id = createCollection(name, color, /*refreshList*/ false);
			return id == LabelId::None ? QString{} : toString(id);  // empty = refused, which the dialog treats as failure
		},
		.viewChanged = [this] { refreshLibraryView(); }
	};

	ImportDialog dialog(std::move(callbacks), Catalog::instance().anySourceDir(), this);
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
	// A media item is "tracked" iff the catalog records it as some item's source path.
	QSet<QString> tracked;
	Catalog& catalog = Catalog::instance();
	for (const Catalog::Entry& entry : catalog.mediaItems())
	{
		if (!entry.sourcePath.isEmpty())
			tracked.insert(pathComparisonKey(entry.sourcePath));
	}

	QSettings settings;
	constexpr const char* lastFolderKey = "untrackedScan/lastFolder";
	const QString defaultStartDir = tracked.empty() ? rootFolder() : QFileInfo(*tracked.begin()).absolutePath();
	const QString startDir = settings.value(lastFolderKey, defaultStartDir).toString();
	const QString dir = QFileDialog::getExistingDirectory(this, tr("Scan folder for untracked media"), startDir);
	if (dir.isEmpty())
		return;
	settings.setValue(lastFolderKey, dir);

	QStringList untracked;
	QDirIterator it(dir, QDir::Files, QDirIterator::Subdirectories);
	while (it.hasNext())
	{
		const QString path = it.next();
		if ((isSupportedVideoFile(path) || isSupportedImageFile(path)) && !tracked.contains(pathComparisonKey(path)))
			untracked.push_back(QDir::toNativeSeparators(path));
	}

	if (untracked.isEmpty())
	{
		QMessageBox::information(this, tr("Scan complete"), tr("No untracked media files were found under:\n%1").arg(QDir::toNativeSeparators(dir)));
		return;
	}

	untracked.sort(Qt::CaseInsensitive);
	importToCollections(untracked);   // ImportDialog's staging is the rich triage UI: preview, remove, label, compare
}

void MainWindow::checkCatalogIntegrity()
{
	IntegrityCheckDialog::Callbacks callbacks{
		.registerRequested = [this](const QString& folderPath, const QString& sourcePath) {
			const bool ok = Catalog::instance().addMediaItem(MediaId::fromFile(sourcePath), sourcePath, folderPath, /*splitIntoFrames=*/true);
			if (ok)
				refreshLibraryView();
			return ok;
		},
		.adoptPhotoRequested = [this](const QString& filePath) {
			// The untracked image already lives in <root>/Photos/<label>/, so adopt it in place as an owned photo
			// under that label (its parent dir). addPhoto ensures the label exists and refuses a name+size clash.
			Catalog& catalog = Catalog::instance();
			const QString labelDir = QFileInfo(filePath).absolutePath();
			const bool ok = catalog.addPhoto(MediaId::fromFile(filePath), filePath, labelDir, /*referenced=*/false);
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
		.locatePhotoRequested = [this](const MediaId& id, const QString& newSourcePath) {
			// Repoint a referenced photo at the file the user located. applyRename carries the record (labels,
			// referenced flag) to the new identity and refuses an id clash; a referenced photo has no storage
			// folder, so it stays folder-less (empty newFolderAbs).
			const bool ok = Catalog::instance().applyRename(id, MediaId::fromFile(newSourcePath), newSourcePath, /*newFolderAbs=*/QString{});
			if (ok)
				refreshLibraryView();
			return ok;
		},
	};
	IntegrityCheckDialog::scanAndShowUi(std::move(callbacks), this);
}

bool MainWindow::isInBest(const MediaId& id)
{
	return Catalog::instance().mediaItemHasLabel(id, Catalog::BestLabelId);
}

void MainWindow::toggleBest(const MediaId& id)
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

void MainWindow::renameItemInteractive(const MediaId& id)
{
	const MediaRename::Result result = MediaRename::renameItemInteractive(id, this);
	if (!result.renamed)
		return;

	refreshLibraryView();
	// A video rename moves its frame folder - follow it if the frame viewer was showing that folder.
	if (!result.oldFolderPath.isEmpty() && m_frameViewer->currentFolder() == result.oldFolderPath && m_frameViewer->isVisible())
		m_frameViewer->showForFolder(result.newFolderPath);
}

void MainWindow::deleteFolderRecursively(const QString& folderPath)
{
	if (!QDir(folderPath).removeRecursively())
		QMessageBox::critical(this, tr("Error"), tr("Failed to delete folder: %1").arg(folderPath));
}
