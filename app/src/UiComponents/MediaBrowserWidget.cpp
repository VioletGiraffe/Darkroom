#include "UiComponents/MediaBrowserWidget.h"
#include "Core/Catalog.h"
#include "Core/Library.h"
#include "Settings.h"
#include "Shortcuts.h"
#include "Theme/Theme.h"
#include "theme/ctintedsvgiconengine.h"
#include "Theme/Style.h"
#include "UiComponents/LabelVisuals.h"
#include "UiComponents/LabelSidebar.h"
#include "UiComponents/MediaGrid.h"
#include "UiComponents/MediaItemWidget.h"
#include "UiComponents/PreviewFrameCountCombo.h"
#include "UiComponents/SegmentedToggle.h"
#include "UiComponents/SortControl.h"
#include "Utils.h"
#include "Windows/CompareWindow.h"
#include "Windows/ImageViewerWindow.h"
#include "Windows/LabelManagement.h"
#include "Windows/MediaItemManagement.h"
#include "Windows/MediaRename.h"
#include "Windows/PhotoCompareWindow.h"
#include "Windows/VideoPlayerWindow.h"

#include "compiler/compiler_warnings_control.h"
#include "utils/naturalsorting/cnaturalsorterqcollator.h"

DISABLE_COMPILER_WARNINGS
#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QKeySequence>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QSet>
#include <QSettings>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
RESTORE_COMPILER_WARNINGS

#include <algorithm>
#include <utility>
#include <vector>

namespace {

// Card width scales with this per-frame height.
const QString CARD_IMAGE_HEIGHT_KEY = QStringLiteral("mainWindow/cardImageHeight");
const QString MEDIA_TYPE_FILTER_KEY = QStringLiteral("mainWindow/mediaTypeFilter");
constexpr int DEFAULT_CARD_IMAGE_HEIGHT = 120;
constexpr int MIN_CARD_IMAGE_HEIGHT = 60;
constexpr int MAX_CARD_IMAGE_HEIGHT = 360;
constexpr int CARD_IMAGE_HEIGHT_STEP = 20;

// Bounds the auto-fit only; a manual drag can exceed it.
constexpr int LABEL_SIDEBAR_MAX_WIDTH = 400;

int cardImageHeight()
{
	return QSettings{}.value(CARD_IMAGE_HEIGHT_KEY, DEFAULT_CARD_IMAGE_HEIGHT).toInt();
}

// Cached so sorting never performs filesystem I/O.
struct ItemInfo
{
	bool isBest = false;
	QDateTime date;
	QString name;
};

ItemInfo itemInfoFor(Catalog& catalog, const MediaId& id, bool isBest, bool sortByDate)
{
	const QString name = catalog.displayName(id);
	return { isBest, sortByDate ? getSourceFileDate(catalog.sourcePathForMediaItem(id), catalog.folderForMediaItem(id)) : QDateTime{}, name };
}

// Sort mode is shared by every item in a sort and set immediately before sortItems().
class GridItem final : public QListWidgetItem {
public:
	static int sortBy;
	static bool descending;
	static bool favoritesFirst;

	static void setSortMode(const SortControl* control)
	{
		sortBy = control->sortBy();
		descending = control->descending();
		favoritesFirst = control->favoritesFirst();
	}

	MediaId mediaId;
	ItemInfo info;
	// Position among the visible rows; kept here because the card that shows it may not exist yet.
	int displayNumber = 0;

	bool operator<(const QListWidgetItem& other) const override
	{
		const ItemInfo& b = static_cast<const GridItem&>(other).info;
		if (favoritesFirst && info.isBest != b.isBest)
			return info.isBest;
		if (sortBy == SortBy::Date)
			return descending ? b.date < info.date : info.date < b.date;

		return descending
			? NaturalSort::lessCaseInsensitive(b.name, info.name)
			: NaturalSort::lessCaseInsensitive(info.name, b.name);
	}
};

int GridItem::sortBy = SortBy::Name;
bool GridItem::descending = false;
bool GridItem::favoritesFirst = false;

QString gridCaption(int displayNumber, const QString& name)
{
	return QString::number(displayNumber) + ":  " + name;
}

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
		dotNames.push_back(label->displayName);
	}

	QString stateLine;
	if (catalog.mediaType(id) == Catalog::MediaType::Video)
		stateLine = (catalog.isSplitIntoFrames(id) ? MediaBrowserWidget::tr("Frames extracted")
		                                         : MediaBrowserWidget::tr("Not extracted yet - middle-click to extract"))
		            + QLatin1String("\n");
	card->setLabelDots(dotColors, stateLine + MediaBrowserWidget::tr("Labels: %1").arg(dotNames.join(", ")));
}

// A card can be built at any moment, so everything it shows must derive from (Catalog, GridItem) alone.
void applyItemStateToCard(Catalog& catalog, const GridItem& item, MediaItemWidget& card)
{
	const MediaId& id = item.mediaId;
	const bool isPhoto = catalog.mediaType(id) == Catalog::MediaType::Photo;

	card.setLabel(gridCaption(item.displayNumber, item.info.name));
	card.setInBest(item.info.isBest);
	card.setFramesExtracted(!isPhoto && catalog.isSplitIntoFrames(id));
	card.setDuration(catalog.durationMsForMediaItem(id));
	applyLabelDots(catalog, id, &card);
}

void renumberGridCaptions(QListWidget* grid)
{
	int visibleNumber = 0;
	for (int row = 0; row < grid->count(); ++row)
	{
		auto* item = static_cast<GridItem*>(grid->item(row));
		if (item->isHidden())
			continue;
		item->displayNumber = ++visibleNumber;
		if (auto* card = static_cast<MediaItemWidget*>(grid->itemWidget(item)))
			card->setLabel(gridCaption(item->displayNumber, item->info.name));
	}
}

std::vector<MediaId> visibleItemsInViewOrder(const QListWidget* grid, const Catalog& catalog, Catalog::MediaType type)
{
	const int count = grid->count();
	std::vector<MediaId> items;
	items.reserve(count);
	for (int row = 0; row < count; ++row)
	{
		const auto* item = static_cast<const GridItem*>(grid->item(row));
		if (item->isHidden() || catalog.mediaType(item->mediaId) != type)
			continue;
		items.push_back(item->mediaId);
	}
	return items;
}

// A leading '^' anchors the otherwise substring-based, case-insensitive match.
bool nameMatchesFilter(const QString& name, const QString& query)
{
	if (query.isEmpty())
		return true;
	if (query.startsWith('^'))
		return name.startsWith(query.mid(1), Qt::CaseInsensitive);
	return name.contains(query, Qt::CaseInsensitive);
}

} // namespace

struct MediaBrowserWidget::GridViewState
{
	QString scrollAnchorKey;
	QSet<QString> selectedKeys;
	QString currentKey;
};

MediaBrowserWidget::MediaBrowserWidget(Library& library, QWidget* parent)
	: QWidget(parent)
	, _library(library)
{
	setupUi();
}

void MediaBrowserWidget::setupUi()
{
	// catalogChanged arrives inside the mutating call stack, so its refresh is queued: rebuilding there would
	// destroy the card or sidebar row whose handler is still running.
	// Zero interval, single shot, coalescing - a multi-item operation's per-item notifications collapse into one rebuild.
	_catalogRefreshTimer = new QTimer(this);
	_catalogRefreshTimer->setSingleShot(true);
	_catalogRefreshTimer->setInterval(0);
	connect(_catalogRefreshTimer, &QTimer::timeout, this, &MediaBrowserWidget::refreshLibraryView);
	connect(&_library, &Library::catalogChanged, this, [this] { _catalogRefreshTimer->start(); });

	auto* rootLayout = new QHBoxLayout(this);
	rootLayout->setContentsMargins(0, 0, 0, 0);
	rootLayout->setSpacing(0);

	_labelSidebar = new LabelSidebar(_library);
	connect(_labelSidebar, &LabelSidebar::filterChanged, this, &MediaBrowserWidget::rebuildGridItems);
	connect(_labelSidebar, &LabelSidebar::addLabelRequested, this, [this] {
		static_cast<void>(LabelManagement::createLabelInteractive(_library.catalog(), window()));
	});
	connect(_labelSidebar, &LabelSidebar::renameLabelRequested, this, [this](LabelId labelId) {
		LabelManagement::renameLabelInteractive(_library.catalog(), labelId, window());
	});
	connect(_labelSidebar, &LabelSidebar::setLabelColorRequested, this, [this](LabelId labelId) {
		LabelManagement::setLabelColorInteractive(_library.catalog(), labelId, window());
	});
	connect(_labelSidebar, &LabelSidebar::deleteLabelRequested, this, [this](LabelId labelId) {
		LabelManagement::deleteLabelInteractive(_library.catalog(), labelId, window());
	});

	auto* rightPanel = new QWidget();
	auto* mainLayout = new QVBoxLayout(rightPanel);
	mainLayout->setContentsMargins(0, 0, 0, 0);
	mainLayout->setSpacing(0);

	auto* headerWidget = new QWidget();
	auto* headerLayout = new QHBoxLayout(headerWidget);
	headerLayout->setContentsMargins(0, 4, 4, 4);
	headerLayout->setSpacing(4);

	_nameFilter = new QLineEdit();
	_nameFilter->setPlaceholderText(tr("filter by name..."));
	_nameFilter->setClearButtonEnabled(true);
	_nameFilter->setMinimumWidth(220);
	_nameFilter->addAction(tintedSvgIcon(QStringLiteral(":/UI/icon_search.svg"), [] { return Theme::current().palette.textDim; }),
		QLineEdit::LeadingPosition);
	connect(_nameFilter, &QLineEdit::textChanged, this, &MediaBrowserWidget::applyNameFilter);
	headerLayout->addWidget(_nameFilter, 0, Qt::AlignVCenter);

	_mediaTypeFilter = new SegmentedToggle({ tr("All"), tr("Videos"), tr("Photos") });
	_mediaTypeFilter->setCurrentIndex(qBound(0, QSettings{}.value(MEDIA_TYPE_FILTER_KEY, 0).toInt(), 2));
	connect(_mediaTypeFilter, &SegmentedToggle::currentChanged, this, [this](int index) {
		QSettings{}.setValue(MEDIA_TYPE_FILTER_KEY, index);
		rebuildGridItems();
	});
	headerLayout->addWidget(_mediaTypeFilter, 0, Qt::AlignVCenter);

	auto* focusFilterShortcut = new QShortcut(QKeySequence("Ctrl+F"), this);
	focusFilterShortcut->setContext(Qt::ApplicationShortcut);
	connect(focusFilterShortcut, &QShortcut::activated, this, [this] {
		window()->raise();
		window()->activateWindow();
		_nameFilter->setFocus();
		_nameFilter->selectAll();
	});

	headerLayout->addStretch(1);

	const int savedFrameCount = QSettings{}.value(Settings::PreviewFrameCount, Defaults::PreviewFrameCount).toInt();
	_previewFrameCountCombo = new PreviewFrameCountCombo(savedFrameCount);

	connect(_previewFrameCountCombo, &QComboBox::currentIndexChanged, this, [this] {
		QSettings{}.setValue(Settings::PreviewFrameCount, previewFrameCount());
		rebuildAllCards();
	});
	headerLayout->addWidget(_previewFrameCountCombo, 0, Qt::AlignVCenter);

	_sortControl = new SortControl();
	connect(_sortControl, &SortControl::changed, this, &MediaBrowserWidget::resortMediaGrid);
	headerLayout->addWidget(_sortControl, 0, Qt::AlignVCenter);

	mainLayout->addWidget(headerWidget);

	_mediaGrid = new MediaGrid();
	// Video and photo cards have different fixed sizes.
	_mediaGrid->setUniformItemSizes(false);
	_mediaGrid->setDragEnabled(true);
	_mediaGrid->setDragUrlsProvider([this](const QList<QListWidgetItem*>& items) { return dragUrlsForItems(items); });
	_mediaGrid->setCardFactory([this](QListWidgetItem* item) { return buildMediaCard(item); });
	Style::applyThemedSheet(_mediaGrid, [] {
		return QStringLiteral("QListWidget::item:selected { background-color: %1; }").arg(Theme::current().palette.accentBg.name());
	});
	connect(_mediaGrid, &QListWidget::itemSelectionChanged, this, &MediaBrowserWidget::selectionChanged);

	mainLayout->addWidget(_mediaGrid, 1);

	// Coalesce wheel steps into one grid rebuild.
	_gridZoomDebounce = new QTimer(this);
	_gridZoomDebounce->setSingleShot(true);
	_gridZoomDebounce->setInterval(80);
	connect(_gridZoomDebounce, &QTimer::timeout, this, &MediaBrowserWidget::rebuildAllCards);

	_splitter = new QSplitter(Qt::Horizontal);
	_splitter->addWidget(_labelSidebar);
	_splitter->addWidget(rightPanel);
	_splitter->setStretchFactor(0, 0);
	_splitter->setStretchFactor(1, 1);
	_splitter->setCollapsible(0, false);
	rootLayout->addWidget(_splitter);

	// Connected last: the fit needs _splitter, and the sidebar can announce a width as soon as it is laid out.
	connect(_labelSidebar, &LabelSidebar::preferredWidthChanged, this, &MediaBrowserWidget::fitLabelSidebarWidth);
}

void MediaBrowserWidget::activateMediaItem(const MediaId& id)
{
	if (_library.catalog().mediaType(id) == Catalog::MediaType::Photo)
		viewPhoto(id);
	else
		playVideo(id);
}

void MediaBrowserWidget::viewPhoto(const MediaId& id)
{
	const Catalog& catalog = _library.catalog();
	const QString sourcePath = catalog.sourcePathForMediaItem(id);
	if (!QFile::exists(sourcePath))
	{
		reportMissingFile(window(), sourcePath);
		return;
	}

	QStringList paths;
	int startIndex = -1;
	for (const MediaId& photoId : visiblePhotosInViewOrder())
	{
		if (photoId == id)
			startIndex = static_cast<int>(paths.size());
		paths << catalog.sourcePathForMediaItem(photoId);
	}

	if (startIndex < 0) // Not in the grid's current view: nothing to browse through, so show it alone
	{
		paths = { sourcePath };
		startIndex = 0;
	}

	ImageViewerWindow::showForImages(&_library, std::move(paths), startIndex, window());
}

void MediaBrowserWidget::playVideo(const MediaId& id)
{
	const QString sourcePath = _library.catalog().sourcePathForMediaItem(id);
	if (!QFile::exists(sourcePath))
	{
		reportMissingFile(window(), sourcePath);
		return;
	}

	auto* playerWindow = new VideoPlayerWindow(_library, sourcePath, id, nullptr);
	playerWindow->setNavigationOrder(visibleVideosInViewOrder());
	playerWindow->show();
}

void MediaBrowserWidget::openSourceInSystemApp(const MediaId& id)
{
	const QString sourcePath = _library.catalog().sourcePathForMediaItem(id);
	if (!QFile::exists(sourcePath))
	{
		reportMissingFile(window(), sourcePath);
		return;
	}

	QDesktopServices::openUrl(QUrl::fromLocalFile(sourcePath));
}

void MediaBrowserWidget::showMediaItemContextMenu(const MediaId& id, const QPoint& globalPos)
{
	Catalog& catalog = _library.catalog();
	const std::vector<MediaId> selection = effectiveSelection(id);
	const QString folderPath = catalog.folderForMediaItem(id);
	const bool isPhoto = catalog.mediaType(id) == Catalog::MediaType::Photo;

	QMenu menu(window());
	const auto addActionWithShortcut = [&menu, this](
			const QString& text, const QKeySequence& shortcut, auto&& slot) {
		QAction* action = menu.addAction(text, this, std::forward<decltype(slot)>(slot));
		action->setShortcut(shortcut);
		action->setShortcutContext(Qt::WidgetShortcut);
	};

	size_t videoCount = 0;
	for (const MediaId& selectedId : selection)
		if (catalog.mediaType(selectedId) == Catalog::MediaType::Video)
			++videoCount;
	const bool selectionAllVideos = videoCount == selection.size();
	const bool selectionAllPhotos = videoCount == 0;

	if (selectionAllVideos)
	{
		menu.addAction(selection.size() > 1 ? tr("Compare selected") : tr("Inspect"), this, [this, selection] {
			QStringList folders;
			for (const MediaId& selectedId : selection)
				folders << _library.catalog().folderForMediaItem(selectedId);
			auto* compareWindow = new CompareWindow(folders, window());
			compareWindow->setAttribute(Qt::WA_DeleteOnClose);
			compareWindow->show();
		});
		menu.addSeparator();
	}

	if (selectionAllPhotos && selection.size() >= 2)
	{
		menu.addAction(tr("Compare photos"), this, [this, selection] {
			QStringList paths;
			for (const MediaId& selectedId : selection)
				paths << _library.catalog().sourcePathForMediaItem(selectedId);
			PhotoCompareWindow::showForFiles(_library, paths, window());
		});
		menu.addSeparator();
	}

	if (!isPhoto)
	{
		menu.addAction(tr("Open frame folder"), this, [folderPath, this] {
			if (!openFolderInFileManager(folderPath))
				reportMissingFile(window(), folderPath);
		});
	}
	menu.addAction(isPhoto ? tr("Open photo") : tr("Play source video"), this, [this, id] {
		openSourceInSystemApp(id);
	});
	menu.addAction(tr("Locate source file"), this, [this, id] {
		const QString sourcePath = _library.catalog().sourcePathForMediaItem(id);
		if (sourcePath.isEmpty())
			QMessageBox::warning(window(), tr("Error"), tr("No source file is recorded for this item."));
		else if (!revealInFileManager(sourcePath))
			reportMissingFile(window(), sourcePath);
	});
	menu.addAction(tr("Copy source path to clipboard"), this, [this, id] {
		const QString sourcePath = _library.catalog().sourcePathForMediaItem(id);
		if (!sourcePath.isEmpty())
			QApplication::clipboard()->setText(QDir::toNativeSeparators(sourcePath));
	});
	addActionWithShortcut(isPhoto ? tr("Rename photo") : tr("Rename media file"), QKeySequence(Shortcuts::Rename),
		[this, id] { renameMediaItemInteractive(id); });
	menu.addSeparator();

	const bool inBest = catalog.mediaItemHasLabel(id, Catalog::BestLabelId);
	menu.addAction(inBest ? tr("Remove from Best") : tr("Add to Best"), this, [this, id] {
		toggleBest(id);
	});

	std::vector<LabelVisuals::ChecklistRow> labelRows;
	for (const Catalog::Label& label : catalog.allLabels())
	{
		if (label.isVirtual())
			continue;
		const LabelId labelId = label.id;

		int haveCount = 0;
		for (const MediaId& selectedId : selection)
			if (catalog.mediaItemHasLabel(selectedId, labelId))
				++haveCount;

		labelRows.push_back({ label.displayName, QColor(label.color),
			LabelVisuals::presenceForCount(haveCount, static_cast<int>(selection.size())),
			[this, selection, labelId](bool addToAll) {
				Catalog::BatchScope batch(_library.catalog());
				for (const MediaId& selectedId : selection)
				{
					if (addToAll)
						_library.catalog().addLabel(selectedId, labelId);
					else
						_library.catalog().removeLabel(selectedId, labelId);
				}
			} });
	}
	LabelVisuals::buildChecklistMenu(menu.addMenu(tr("Labels")), std::move(labelRows));
	menu.addSeparator();

	addActionWithShortcut(
		selection.size() > 1
			? tr("Remove %1 items from library (untrack)").arg(selection.size())
			: tr("Remove from library (untrack)"),
		QKeySequence(Shortcuts::RemoveFromList), [this, selection] { removeMediaItemsFromLibraryInteractive(selection); });
	addActionWithShortcut(selection.size() > 1 ? tr("Delete (%1 items)").arg(selection.size()) : tr("Delete"),
		QKeySequence(Shortcuts::DeleteFile), [this, selection] { deleteMediaItemsInteractive(selection); });

	menu.exec(globalPos);
}

void MediaBrowserWidget::deleteMediaItemsInteractive(const std::vector<MediaId>& selection)
{
	const MediaItemManagement::DeleteResult result =
		MediaItemManagement::deleteItemsInteractive(_library.catalog(), selection, window());
	if (result.storageRefreshRequired)
		refreshLibraryView();
	for (const QString& folderPath : result.affectedFrameFolders)
		emit frameFolderPathChanged(folderPath, {}, {});
}

void MediaBrowserWidget::removeMediaItemsFromLibraryInteractive(const std::vector<MediaId>& selection)
{
	MediaItemManagement::removeItemsFromLibraryInteractive(_library.catalog(), selection, window());
}

void MediaBrowserWidget::renameMediaItemInteractive(const MediaId& id)
{
	const MediaRename::Result result = MediaRename::renameItemInteractive(_library.catalog(), id, window());
	if (!result.renamed)
		return;

	if (!result.oldFolderPath.isEmpty())
		emit frameFolderPathChanged(result.oldFolderPath, result.newFolderPath, result.newName);
}

void MediaBrowserWidget::deleteSelectedMediaItemsInteractive()
{
	deleteMediaItemsInteractive(selectedMediaItems());
}

void MediaBrowserWidget::removeSelectedMediaItemsFromLibraryInteractive()
{
	removeMediaItemsFromLibraryInteractive(selectedMediaItems());
}

void MediaBrowserWidget::renameSelectedMediaItemInteractive()
{
	const std::vector<MediaId> selection = selectedMediaItems();
	if (selection.size() == 1)
		renameMediaItemInteractive(selection.front());
}

void MediaBrowserWidget::saveSettings()
{
	QVariantList activeIds;
	for (const LabelId id : _labelSidebar->activeLabelIds())
		activeIds.push_back(static_cast<qulonglong>(toUInt64(id)));
	QSettings{}.setValue("mainWindow/activeLabelIds", activeIds);
	QSettings{}.setValue("mainWindow/labelsAndMode", _labelSidebar->isAndMode());
	QSettings{}.setValue("mainWindow/scrollAnchor", topAnchorKey());
}

void MediaBrowserWidget::restoreSettings()
{
	QList<LabelId> activeIds;
	for (const QVariant& v : QSettings{}.value("mainWindow/activeLabelIds").toList())
		activeIds.push_back(labelIdFromUInt64(v.toULongLong()));
	const bool andMode = QSettings{}.value("mainWindow/labelsAndMode", false).toBool();
	_labelSidebar->setActiveFilter(activeIds, andMode);

	const QString scrollAnchorKey = QSettings{}.value("mainWindow/scrollAnchor").toString();
	// The wrapped grid's final geometry is available only after post-show resize events. Build rows then, restore
	// the persisted position against that layout, and only afterward materialize the cards around it.
	QMetaObject::invokeMethod(this, [this, scrollAnchorKey] {
		QElapsedTimer timer;
		timer.start();

		rebuildGridRows();
		scrollGridToAnchorKey(scrollAnchorKey);
		_mediaGrid->ensureVisibleCardsExist();
	}, Qt::QueuedConnection);
}

void MediaBrowserWidget::resetForLibrarySwitch()
{
	_mediaGrid->clear();
	_labelSidebar->setActiveFilter({}, _labelSidebar->isAndMode());
}

// Keep sidebar rebuilding out of rebuildGridItems(): that function can run from a sidebar item's click signal.
void MediaBrowserWidget::refreshLibraryView()
{
	_catalogRefreshTimer->stop(); // drops a queued repeat of the work done here
	rebuildGridItems();
	_labelSidebar->refresh();
}

void MediaBrowserWidget::fitLabelSidebarWidth()
{
	const QList<int> paneWidths = _splitter->sizes();
	const int fitted = qMax(paneWidths[0], qMin(_labelSidebar->sizeHint().width(), LABEL_SIDEBAR_MAX_WIDTH));
	if (fitted == paneWidths[0])
		return;

	_splitter->setSizes({ fitted, paneWidths[0] + paneWidths[1] - fitted });
}

int MediaBrowserWidget::previewFrameCount() const
{
	return _previewFrameCountCombo->frameCount();
}

void MediaBrowserWidget::installGridAction(QAction* action)
{
	_mediaGrid->addAction(action);
}

bool MediaBrowserWidget::isGridDragSource(const QObject* source) const
{
	return source == _mediaGrid;
}

void MediaBrowserWidget::zoomCards(int steps)
{
	const int current = cardImageHeight();
	const int next = qBound(MIN_CARD_IMAGE_HEIGHT, current + steps * CARD_IMAGE_HEIGHT_STEP, MAX_CARD_IMAGE_HEIGHT);
	if (next == current)
		return;

	QSettings{}.setValue(CARD_IMAGE_HEIGHT_KEY, next);
	_gridZoomDebounce->start();
}

std::vector<MediaId> MediaBrowserWidget::mediaItemsMatchingFilters() const
{
	const Catalog& catalog = _library.catalog();

	const QList<LabelId> activeLabelIds = _labelSidebar->activeLabelIds();
	std::vector<MediaId> mediaItems;
	if (activeLabelIds.empty())
	{
		const auto& all = catalog.mediaItems();
		mediaItems.assign(all.keyBegin(), all.keyEnd());
	}
	else
	{
		QSet<MediaId> matched = catalog.mediaItemsForLabel(activeLabelIds.front());
		for (qsizetype i = 1; i < activeLabelIds.size(); ++i)
		{
			const QSet<MediaId> next = catalog.mediaItemsForLabel(activeLabelIds[i]);
			if (_labelSidebar->isAndMode())
				matched.intersect(next);
			else
				matched.unite(next);
		}
		mediaItems.assign(matched.cbegin(), matched.cend());
	}

	if (const int typeFilterIdx = _mediaTypeFilter->currentIndex(); typeFilterIdx != 0)
	{
		const Catalog::MediaType wanted = typeFilterIdx == 2 ? Catalog::MediaType::Photo : Catalog::MediaType::Video;
		std::erase_if(mediaItems, [&](const MediaId& id) { return catalog.mediaType(id) != wanted; });
	}

	return mediaItems;
}

MediaItemWidget* MediaBrowserWidget::buildMediaCard(QListWidgetItem* item)
{
	const auto& gridItem = *static_cast<GridItem*>(item);
	const MediaId& id = gridItem.mediaId;

	Catalog& catalog = _library.catalog();
	const bool isPhoto = catalog.mediaType(id) == Catalog::MediaType::Photo;
	QStringList previewPaths;
	if (isPhoto)
	{
		previewPaths.push_back(catalog.sourcePathForMediaItem(id));
	}
	else
	{
		// Fall back to full-size frames when the preview cache is missing.
		const QString folderPath = catalog.folderForMediaItem(id);
		QDir frameSource(Catalog::previewDirFor(folderPath));
		QStringList imageFiles = listFrameImageFiles(frameSource);
		if (imageFiles.empty())
		{
			frameSource.setPath(folderPath);
			imageFiles = listFrameImageFiles(frameSource);
		}

		previewPaths = pickEvenlySpacedFrames(frameSource, imageFiles, previewFrameCount());
	}

	auto* card = new MediaItemWidget(
		isPhoto ? _photoCanvas : _videoCanvas,
		previewPaths, QString(),
		id,
		gridItem.info.isBest,
		[this, id] { toggleBest(id); },
		[this, id] { activateMediaItem(id); },
		[this, id](QPoint globalPos) { showMediaItemContextMenu(id, globalPos); },
		/* dynamic size hint */false,
		/* film strip */ !isPhoto
	);
	if (!isPhoto)
		card->setOnMiddleButtonClick([this, id] { emit inspectVideoFramesRequested(id); });
	card->setOnMouseWheelCallback([this](int steps) { zoomCards(steps); });

	// Defer rebuilding the grid until this card's dropEvent has unwound.
	card->setOnLabelDropped([this, id](const QString& labelId) {
		const std::vector<MediaId> targets = effectiveSelection(id);
		const LabelId dropped = labelIdFromString(labelId);
		const uint64_t libraryGeneration = _library.generation();
		QMetaObject::invokeMethod(this, [this, libraryGeneration, targets, dropped] {
			if (_library.generation() != libraryGeneration)
				return;
			Catalog::BatchScope batch(_library.catalog());
			for (const MediaId& target : targets)
				_library.catalog().addLabel(target, dropped);
		}, Qt::QueuedConnection);
	});

	applyItemStateToCard(catalog, gridItem, *card);
	return card;
}

void MediaBrowserWidget::updateCardCanvasSizes()
{
	const int imageHeight = cardImageHeight();
	// Make each video span the same grid width as previewFrameCount photo cards.
	_photoCanvas = { imageHeight, imageHeight };
	_videoCanvas = { MediaItemWidget::videoCanvasWidthForTiling(imageHeight, previewFrameCount(), _mediaGrid->spacing()), imageHeight };
}

// Every card of one media type is the same size, and rows need their hint before any card exists.
QSize MediaBrowserWidget::cardSizeHintFor(bool isPhoto) const
{
	MediaItemWidget probe{
		isPhoto ? _photoCanvas : _videoCanvas,
		{}, QString(),
		MediaId{},
		false,
		{}, {}, {},
		/* dynamic size hint */ false,
		/* film strip */ !isPhoto
	};
	return probe.sizeHint();
}

void MediaBrowserWidget::applyCardSizeHints()
{
	const Catalog& catalog = _library.catalog();
	const QSize photoCardHint = cardSizeHintFor(/* isPhoto */ true);
	const QSize videoCardHint = cardSizeHintFor(/* isPhoto */ false);
	for (int row = 0, rows = _mediaGrid->count(); row < rows; ++row)
	{
		auto* item = static_cast<GridItem*>(_mediaGrid->item(row));
		item->setSizeHint(catalog.mediaType(item->mediaId) == Catalog::MediaType::Photo ? photoCardHint : videoCardHint);
	}
}

void MediaBrowserWidget::rebuildGridItems()
{
	const GridViewState viewState = captureGridViewState();
	rebuildGridRows();

	// Restore against the final post-filter layout; materialize only then, so no cards are built at the pre-restore scroll position.
	restoreGridViewState(viewState);
	_mediaGrid->ensureVisibleCardsExist();
}

void MediaBrowserWidget::rebuildGridRows()
{
	_mediaGrid->clear();

	updateCardCanvasSizes();

	Catalog& catalog = _library.catalog();
	const QSet<MediaId> bestSet = catalog.mediaItemsForLabel(Catalog::BestLabelId);

	GridItem::setSortMode(_sortControl);
	const bool sortByDate = GridItem::sortBy == SortBy::Date;

	const std::vector<MediaId> mediaItems = mediaItemsMatchingFilters();

	_mediaGrid->setEmptyMessage(catalog.mediaItemCount() == 0
		? tr("The library is empty.\nDrop media files here, or use Tools > Import.")
		: tr("No items match the current filters."));

	const QSize photoCardHint = cardSizeHintFor(/* isPhoto */ true);
	const QSize videoCardHint = cardSizeHintFor(/* isPhoto */ false);
	for (const MediaId& id : mediaItems)
	{
		auto* item = new GridItem();
		item->mediaId = id;
		item->info = itemInfoFor(catalog, id, bestSet.contains(id), sortByDate);
		item->setSizeHint(catalog.mediaType(id) == Catalog::MediaType::Photo ? photoCardHint : videoCardHint);
		_mediaGrid->addItem(item);
	}

	_mediaGrid->sortItems(Qt::AscendingOrder);
	applyNameFilterToRows();
}

void MediaBrowserWidget::rebuildAllCards()
{
	updateCardCanvasSizes();
	applyCardSizeHints();
	_mediaGrid->discardAllCards();
	_mediaGrid->ensureVisibleCardsExist();
}

QString MediaBrowserWidget::topAnchorKey() const
{
	for (int row = 0; row < _mediaGrid->count(); ++row)
	{
		const QListWidgetItem* item = _mediaGrid->item(row);
		if (!item->isHidden() && _mediaGrid->visualItemRect(item).bottom() >= 0)
			return static_cast<const GridItem*>(item)->mediaId.key();
	}
	return {};
}

void MediaBrowserWidget::scrollGridToAnchorKey(const QString& anchorKey)
{
	if (anchorKey.isEmpty())
		return;
	for (int row = 0; row < _mediaGrid->count(); ++row)
	{
		QListWidgetItem* item = _mediaGrid->item(row);
		if (!item->isHidden() && static_cast<const GridItem*>(item)->mediaId.key() == anchorKey)
		{
			_mediaGrid->scrollToItem(item, QAbstractItemView::PositionAtTop);
			return;
		}
	}
}

MediaBrowserWidget::GridViewState MediaBrowserWidget::captureGridViewState() const
{
	GridViewState state;
	state.scrollAnchorKey = topAnchorKey();
	for (const QListWidgetItem* item : _mediaGrid->selectedItems())
		state.selectedKeys.insert(static_cast<const GridItem*>(item)->mediaId.key());
	if (const QListWidgetItem* current = _mediaGrid->currentItem())
		state.currentKey = static_cast<const GridItem*>(current)->mediaId.key();
	return state;
}

void MediaBrowserWidget::restoreGridViewState(const GridViewState& state)
{
	if (!state.selectedKeys.empty() || !state.currentKey.isEmpty())
	{
		QListWidgetItem* currentItem = nullptr;
		{
			const QSignalBlocker blocker{ _mediaGrid };
			for (int row = 0; row < _mediaGrid->count(); ++row)
			{
				auto* item = static_cast<GridItem*>(_mediaGrid->item(row));
				if (item->isHidden())
					continue;
				const QString key = item->mediaId.key();
				if (state.selectedKeys.contains(key))
					item->setSelected(true);
				if (!state.currentKey.isEmpty() && key == state.currentKey)
					currentItem = item;
			}
			if (currentItem)
				_mediaGrid->setCurrentItem(currentItem, QItemSelectionModel::NoUpdate);
		}
		emit selectionChanged();
	}

	scrollGridToAnchorKey(state.scrollAnchorKey);
}

// Reorder existing cards to avoid re-decoding their thumbnails.
void MediaBrowserWidget::resortMediaGrid()
{
	const int count = _mediaGrid->count();
	if (count == 0)
		return;

	Catalog& catalog = _library.catalog();
	const QSet<MediaId> bestSet = catalog.mediaItemsForLabel(Catalog::BestLabelId);

	GridItem::setSortMode(_sortControl);
	const bool sortByDate = GridItem::sortBy == SortBy::Date;

	for (int row = 0; row < count; ++row)
	{
		auto* item = static_cast<GridItem*>(_mediaGrid->item(row));
		item->info = itemInfoFor(catalog, item->mediaId, bestSet.contains(item->mediaId), sortByDate);
	}

	_mediaGrid->sortItems(Qt::AscendingOrder);
	renumberGridCaptions(_mediaGrid);
	_mediaGrid->ensureVisibleCardsExist();
}

void MediaBrowserWidget::applyNameFilterToRows()
{
	const QString query = _nameFilter->text().trimmed();
	for (int row = 0; row < _mediaGrid->count(); ++row)
	{
		auto* item = static_cast<GridItem*>(_mediaGrid->item(row));
		const bool hide = !nameMatchesFilter(item->info.name, query);
		item->setHidden(hide);
		if (hide)
			item->setSelected(false);
	}
	renumberGridCaptions(_mediaGrid);
}

void MediaBrowserWidget::applyNameFilter()
{
	applyNameFilterToRows();
	_mediaGrid->ensureVisibleCardsExist();
}

std::vector<MediaId> MediaBrowserWidget::selectedMediaItems() const
{
	const QList<QListWidgetItem*> selectedItems = _mediaGrid->selectedItems();
	std::vector<MediaId> selected;
	selected.reserve(selectedItems.size());
	for (const QListWidgetItem* item : selectedItems)
		selected.push_back(static_cast<const GridItem*>(item)->mediaId);
	return selected;
}

std::vector<MediaId> MediaBrowserWidget::visibleVideosInViewOrder() const
{
	return visibleItemsInViewOrder(_mediaGrid, _library.catalog(), Catalog::MediaType::Video);
}

std::vector<MediaId> MediaBrowserWidget::visiblePhotosInViewOrder() const
{
	return visibleItemsInViewOrder(_mediaGrid, _library.catalog(), Catalog::MediaType::Photo);
}

std::vector<MediaId> MediaBrowserWidget::effectiveSelection(std::optional<MediaId> target) const
{
	std::vector<MediaId> selected = selectedMediaItems();
	if (target && std::find(selected.cbegin(), selected.cend(), *target) == selected.cend())
		selected = { *target };
	return selected;
}

QList<QUrl> MediaBrowserWidget::dragUrlsForItems(const QList<QListWidgetItem*>& items) const
{
	const Catalog& catalog = _library.catalog();
	QList<QUrl> urls;
	urls.reserve(items.size());
	for (const QListWidgetItem* item : items)
	{
		const QString path = catalog.sourcePathForMediaItem(static_cast<const GridItem*>(item)->mediaId);
		if (!path.isEmpty() && QFile::exists(path))
			urls.push_back(QUrl::fromLocalFile(path));
	}
	return urls;
}

void MediaBrowserWidget::toggleBest(const MediaId& id)
{
	Catalog& catalog = _library.catalog();
	if (catalog.mediaItemHasLabel(id, Catalog::BestLabelId))
		catalog.removeLabel(id, Catalog::BestLabelId);
	else
		catalog.addLabel(id, Catalog::BestLabelId);
}
