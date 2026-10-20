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
#include <QResizeEvent>
#include <QScrollBar>
#include <QStyle>
#include <QUrl>
#include <QWheelEvent>
RESTORE_COMPILER_WARNINGS

#include <algorithm>
#include <utility>

namespace {

static constexpr int MAX_DRAG_IMAGE_EDGE = 180;
static constexpr int PRELOAD_VISUAL_ROWS_PER_SIDE = 3;

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

// A dynamic property change recomputes no stylesheet rule by itself, and the rule cache is per widget: descendants
// styled through the card's state must be repolished along with it.
void setCardSelected(QWidget& card, bool selected)
{
	card.setProperty("selected", selected);
	QList<QWidget*> widgets = card.findChildren<QWidget*>();
	widgets.push_back(&card);
	for (QWidget* w : widgets)
	{
		w->style()->unpolish(w);
		w->style()->polish(w);
	}
	card.update();
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
	setSpacing(10);
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
	font.setPointSizeF(font.pointSizeF() + 2);
	p.setFont(font);
	p.drawText(viewport()->rect().adjusted(20, 20, -20, -20), Qt::AlignCenter | Qt::TextWordWrap, _emptyMessage);
}

void MediaGrid::wheelEvent(QWheelEvent* event)
{
	// QListView resets the scrollbar step to the row height on every layout pass.
	const int dy = event->angleDelta().y();
	if (dy != 0)
	{
		verticalScrollBar()->setValue(verticalScrollBar()->value() - dy);
		event->accept();
		return;
	}
	QListWidget::wheelEvent(event);
}

void MediaGrid::resizeEvent(QResizeEvent* event)
{
	QListWidget::resizeEvent(event);
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
