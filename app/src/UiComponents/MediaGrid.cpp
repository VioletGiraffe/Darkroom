#include "UiComponents/MediaGrid.h"

#include <QColor>
#include <QCursor>
#include <QDrag>
#include <QFont>
#include <QMimeData>
#include <QPainter>
#include <QPixmap>
#include <QUrl>

namespace {

// Largest edge of the card drag image; a bigger card is scaled down so the drag cursor stays a reasonable size.
static constexpr int MAX_DRAG_IMAGE_EDGE = 180;

// Draws a small count badge onto the top-right of the drag image to signal that more than one file is being
// dragged (mirrors the card's split-pending badge look: translucent black disc, white glyph).
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

void MediaGrid::setDragUrlsProvider(std::function<QList<QUrl>(const QList<QListWidgetItem*>&)> provider)
{
	m_dragUrlsProvider = std::move(provider);
}

void MediaGrid::startDrag(Qt::DropActions /*supportedActions*/)
{
	// The view already resolved the selection on press (a plain press on an unselected card selects just it; a
	// press within a multi-selection keeps the group - see the setDragEnabled note in MainWindow::setupUI), so
	// the current selection is exactly what the user grabbed.
	const QList<QListWidgetItem*> items = selectedItems();
	if (items.isEmpty() || !m_dragUrlsProvider)
		return;

	const QList<QUrl> urls = m_dragUrlsProvider(items);
	if (urls.isEmpty())
		return;   // nothing to export (e.g. every selected file is missing / on an unmounted drive)

	auto* mime = new QMimeData();
	mime->setUrls(urls);

	auto* drag = new QDrag(this);
	drag->setMimeData(mime);

	// Drag image: the card actually under the cursor (falls back to the first selected one).
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

	// Copy only: dragging a card out must never move or delete the original in the library.
	drag->exec(Qt::CopyAction);
}
