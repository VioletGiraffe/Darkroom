#include "UiComponents/MediaGrid.h"
#include "Theme/Theme.h"

#include <QColor>
#include <QCursor>
#include <QDrag>
#include <QFont>
#include <QMimeData>
#include <QPainter>
#include <QPixmap>
#include <QResizeEvent>
#include <QScrollBar>
#include <QUrl>
#include <QWheelEvent>

#include <algorithm>

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

}  // namespace

MediaGrid::MediaGrid(QWidget* parent)
	: QListWidget(parent)
{
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
			setItemWidget(rowItem, _cardFactory(rowItem));
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
	p.setPen(QColor(QString::fromLatin1(Theme::current().InstructionText)));
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

	drag->exec(Qt::CopyAction);
}
