#include "UiComponents/MediaGrid.h"
#include "Theme/Theme.h"
#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QColor>
#include <QCursor>
#include <QDrag>
#include <QFont>
#include <QItemSelection>
#include <QItemSelectionModel>
#include <QMimeData>
#include <QModelIndex>
#include <QPainter>
#include <QPixmap>
#include <QRect>
#include <QResizeEvent>
#include <QScrollBar>
#include <QStyle>
#include <QUrl>
#include <QWheelEvent>
RESTORE_COMPILER_WARNINGS

#include <algorithm>
#include <cmath>
#include <utility>

namespace {

static constexpr int MAX_DRAG_IMAGE_EDGE = 180;
static constexpr int PRELOAD_VISUAL_ROWS_PER_SIDE = 3;
static constexpr int EMPTY_MESSAGE_POINT_SIZE = 16;

void paintDragCountBadge(QPixmap& pixmap, int count)
{
	QPainter p{ &pixmap };
	p.setRenderHint(QPainter::Antialiasing);

	static constexpr int DIAMETER = 22;
	static constexpr int MARGIN = 4;
	const QRect badge{ pixmap.width() - DIAMETER - MARGIN, MARGIN, DIAMETER, DIAMETER };

	p.setPen(Qt::NoPen);
	p.setBrush(QColor(0, 0, 0, 200));
	p.drawEllipse(badge);

	QFont font = p.font();
	font.setBold(true);
	p.setFont(font);
	p.setPen(QColor(255, 255, 255));
	p.drawText(badge, Qt::AlignCenter, QString::number(count));
}

// A dynamic property change recomputes no stylesheet rule by itself.
// Only the card is repolished, so no stylesheet rule may style a card's descendants by its state.
void setCardSelected(QWidget& card, bool selected)
{
	card.setProperty("selected", selected);
	card.style()->unpolish(&card);
	card.style()->polish(&card);
	card.update();
}

struct ViewportAnchor
{
	QListWidgetItem* item = nullptr;
	int top = 0;   // viewport coordinates
};

// Rows flow left to right, so the first shown row reaching into the viewport is the top-left visible item.
ViewportAnchor firstVisibleItem(const QListWidget& grid)
{
	for (int row = 0, rows = grid.count(); row < rows; ++row)
	{
		QListWidgetItem* rowItem = grid.item(row);
		if (rowItem->isHidden())
			continue;
		if (const QRect rect = grid.visualItemRect(rowItem); rect.bottom() >= 0)
			return { rowItem, rect.top() };
	}
	return {};
}

}  // namespace

MediaGrid::MediaGrid(QWidget* parent)
	: QListWidget(parent)
{
	setObjectName("mediaGrid");   // the central sheet styles this grid's cells; an owner may rename for its own cell rules
	setViewMode(QListView::IconMode);
	setFlow(QListView::LeftToRight);
	setWrapping(true);
	setResizeMode(QListView::Adjust);
	setMovement(QListView::Static);
	setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
	setSelectionMode(QAbstractItemView::ExtendedSelection);
	setSpacing(Theme::current().metrics.mediaGridSpacing);
}

void MediaGrid::setDragUrlsProvider(std::function<QList<QUrl>(const QList<QListWidgetItem*>&)> provider)
{
	_dragUrlsProvider = std::move(provider);
}

void MediaGrid::setDragActions(Qt::DropActions actions)
{
	_dragActions = actions;
}

void MediaGrid::setDragFinishedHandler(std::function<void(Qt::DropAction action, const QList<QUrl>& urls)> handler)
{
	_dragFinishedHandler = std::move(handler);
}

void MediaGrid::setCardFactory(std::function<QWidget*(QListWidgetItem*)> factory)
{
	_cardFactory = std::move(factory);
}

void MediaGrid::ensureVisibleCardsExist()
{
	if (!_cardFactory)
		return;

	// visualItemRect() reads the item layout, which QListView otherwise defers to the next event loop pass.
	executeDelayedItemsLayout();

	const int rows = count();
	// Browser cards share one fixed height, so a pixel margin of three row pitches cannot reach a fourth off-screen row.
	const int rowPitch = rows == 0 ? 0 : item(0)->sizeHint().height() + spacing();
	const int slack = std::min(viewport()->height(), PRELOAD_VISUAL_ROWS_PER_SIDE * rowPitch);
	const QRect visibleArea = viewport()->rect();
	const QRect materializeArea = visibleArea.adjusted(0, -slack, 0, slack);

	const auto materializeCardsIn = [this, rows](const QRect& area) {
		for (int row = 0; row < rows; ++row)
		{
			QListWidgetItem* rowItem = item(row);
			if (rowItem->isHidden() || !area.intersects(visualItemRect(rowItem)) || itemWidget(rowItem))
				continue;
			QWidget* card = _cardFactory(rowItem);
			card->setProperty("selected", rowItem->isSelected());   // read by the first polish, so no repolish needed
			setItemWidget(rowItem, card);
		}
	};

	// Start the useful work in view order before filling the bounded off-screen preload margin.
	materializeCardsIn(visibleArea);
	materializeCardsIn(materializeArea);
}

void MediaGrid::discardAllCards()
{
	// setItemWidget deletes the widget it replaces, so a null one clears the row.
	for (int row = 0, rows = count(); row < rows; ++row)
		setItemWidget(item(row), nullptr);
}

void MediaGrid::filterRows(const std::function<bool(const QListWidgetItem&)>& isShown)
{
	QItemSelection hiddenSelected;
	for (int row = 0, rows = count(); row < rows; ++row)
	{
		QListWidgetItem* rowItem = item(row);
		const bool hide = !isShown(*rowItem);
		rowItem->setHidden(hide);
		if (hide && rowItem->isSelected())
		{
			const QModelIndex index = indexFromItem(rowItem);
			hiddenSelected.select(index, index);
		}
	}
	selectionModel()->select(hiddenSelected, QItemSelectionModel::Deselect);

	if (const QListWidgetItem* current = currentItem(); current && current->isHidden())
		selectionModel()->setCurrentIndex({}, QItemSelectionModel::NoUpdate);
}

QList<QListWidgetItem*> MediaGrid::effectiveSelection(QListWidgetItem& item)
{
	if (!item.isSelected())
		return { &item };
	return item.listWidget()->selectedItems();
}

void MediaGrid::setEmptyMessage(const QString& message)
{
	if (_emptyMessage == message)
		return;
	_emptyMessage = message;
	viewport()->update();
}

void MediaGrid::paintEvent(QPaintEvent* event)
{
	QListWidget::paintEvent(event);

	if (_emptyMessage.isEmpty())
		return;
	for (int row = 0; row < count(); ++row)
		if (!item(row)->isHidden())
			return;

	QPainter p{ viewport() };
	p.setPen(Theme::current().instructionText);
	QFont font = p.font();
	font.setPointSize(EMPTY_MESSAGE_POINT_SIZE);
	p.setFont(font);
	p.drawText(viewport()->rect().adjusted(20, 20, -20, -20), Qt::AlignCenter | Qt::TextWordWrap, _emptyMessage);
}

void MediaGrid::setZoomHandler(std::function<void(int steps)> handler)
{
	_zoomHandler = std::move(handler);
}

int MediaGrid::zoomedSize(int size, int steps, int minSize, int maxSize)
{
	return qBound(minSize, static_cast<int>(std::lround(size * std::pow(1.1, steps))), maxSize);
}

void MediaGrid::wheelEvent(QWheelEvent* event)
{
	const int dy = event->angleDelta().y();
	if (dy == 0)
	{
		QListWidget::wheelEvent(event);
		return;
	}

	if (_zoomHandler && (event->modifiers() & Qt::ControlModifier))
		_zoomHandler(dy > 0 ? 1 : -1);
	else
	{
		// Scrolls by the raw delta: QListView resets the scrollbar step to the row height on every layout pass.
		verticalScrollBar()->setValue(verticalScrollBar()->value() - dy);
	}
	event->accept();
}

void MediaGrid::resizeEvent(QResizeEvent* event)
{
	// Read before the base handler, which reflows the items for the new width.
	const ViewportAnchor anchor = event->size().width() != event->oldSize().width() ? firstVisibleItem(*this) : ViewportAnchor{};
	QListWidget::resizeEvent(event);
	if (anchor.item)
	{
		executeDelayedItemsLayout();   // QListView may defer the reflow
		verticalScrollBar()->setValue(verticalScrollBar()->value() + visualItemRect(anchor.item).top() - anchor.top);
	}
	ensureVisibleCardsExist();
}

void MediaGrid::scrollContentsBy(int dx, int dy)
{
	QListWidget::scrollContentsBy(dx, dy);
	ensureVisibleCardsExist();
}

void MediaGrid::selectionChanged(const QItemSelection& selected, const QItemSelection& deselected)
{
	QListWidget::selectionChanged(selected, deselected);

	for (const QItemSelection* delta : { &selected, &deselected })
		for (const QModelIndex& index : delta->indexes())
			if (QWidget* card = itemWidget(itemFromIndex(index)))
				setCardSelected(*card, delta == &selected);

	// Select-all and Shift range selections span hidden rows.
	QItemSelection hiddenSelected;
	for (const QModelIndex& index : selected.indexes())
		if (isIndexHidden(index))
			hiddenSelected.select(index, index);
	if (!hiddenSelected.isEmpty())
		selectionModel()->select(hiddenSelected, QItemSelectionModel::Deselect);
}

void MediaGrid::startDrag(Qt::DropActions /*supportedActions*/)
{
	const QList<QListWidgetItem*> items = selectedItems();
	if (items.isEmpty() || !_dragUrlsProvider)
		return;

	const QList<QUrl> urls = _dragUrlsProvider(items);
	if (urls.isEmpty())
		return;

	auto* mime = new QMimeData();
	mime->setUrls(urls);

	auto* drag = new QDrag(this);
	drag->setMimeData(mime);

	QListWidgetItem* grabbed = itemAt(viewport()->mapFromGlobal(QCursor::pos()));
	if (!grabbed || !grabbed->isSelected())
		grabbed = items.first();
	if (QWidget* card = itemWidget(grabbed))
	{
		QPixmap pixmap = card->grab();
		if (pixmap.width() > MAX_DRAG_IMAGE_EDGE || pixmap.height() > MAX_DRAG_IMAGE_EDGE)
			pixmap = pixmap.scaled(MAX_DRAG_IMAGE_EDGE, MAX_DRAG_IMAGE_EDGE, Qt::KeepAspectRatio, Qt::SmoothTransformation);
		if (urls.size() > 1)
			paintDragCountBadge(pixmap, static_cast<int>(urls.size()));
		drag->setPixmap(pixmap);
		drag->setHotSpot(QPoint(pixmap.width() / 2, pixmap.height() / 2));
	}

	const Qt::DropAction result = drag->exec(_dragActions);
	if (_dragFinishedHandler)
		_dragFinishedHandler(result, urls);
}
