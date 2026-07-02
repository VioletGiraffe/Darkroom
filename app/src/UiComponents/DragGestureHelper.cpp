#include "UiComponents/DragGestureHelper.h"

#include <QApplication>
#include <QDrag>
#include <QMimeData>
#include <QMouseEvent>

void DragGestureHelper::mousePressed(const QMouseEvent* event)
{
	if (event->button() == Qt::LeftButton)
		m_pressPos = event->pos();
}

static constexpr QSize MAX_DRAG_PIXMAP_SIZE{ 120, 120 };

bool DragGestureHelper::tryStartDrag(QWidget* widget, const QMouseEvent* moveEvent,
                                     const std::function<QMimeData*()>& makeMimeData, Qt::DropAction action,
                                     const QPixmap& dragPixmap)
{
	if (!(moveEvent->buttons() & Qt::LeftButton))
		return false;
	if ((moveEvent->pos() - m_pressPos).manhattanLength() < QApplication::startDragDistance())
		return false;

	auto* drag = new QDrag(widget);
	drag->setMimeData(makeMimeData());

	const QPixmap pm = dragPixmap.isNull() ? widget->grab() : dragPixmap;
	if (!pm.isNull())
	{
		const bool tooBig = pm.width() > MAX_DRAG_PIXMAP_SIZE.width() || pm.height() > MAX_DRAG_PIXMAP_SIZE.height();
		drag->setPixmap(tooBig ? pm.scaled(MAX_DRAG_PIXMAP_SIZE, Qt::KeepAspectRatio, Qt::SmoothTransformation) : pm);
	}

	drag->exec(action);
	return true;
}
