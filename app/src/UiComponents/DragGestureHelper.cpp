#include "UiComponents/DragGestureHelper.h"

#include <QApplication>
#include <QDrag>
#include <QListWidget>
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

	QMimeData* mimeData = makeMimeData();
	if (!mimeData)
		return false;  // vetoed: this press is not a valid drag source

	auto* drag = new QDrag(widget);
	drag->setMimeData(mimeData);

	const QPixmap pm = dragPixmap.isNull() ? widget->grab() : dragPixmap;
	if (!pm.isNull())
	{
		const bool tooBig = pm.width() > MAX_DRAG_PIXMAP_SIZE.width() || pm.height() > MAX_DRAG_PIXMAP_SIZE.height();
		drag->setPixmap(tooBig ? pm.scaled(MAX_DRAG_PIXMAP_SIZE, Qt::KeepAspectRatio, Qt::SmoothTransformation) : pm);
	}

	drag->exec(action);
	return true;
}

ListRowDragFilter::ListRowDragFilter(QListWidget* list, std::function<QMimeData*(const QListWidgetItem*)> makeMimeDataForRow)
	: QObject(list)
	, m_list(list)
	, m_makeMimeDataForRow(std::move(makeMimeDataForRow))
{
	m_list->viewport()->installEventFilter(this);
}

bool ListRowDragFilter::eventFilter(QObject* watched, QEvent* event)
{
	switch (event->type())
	{
	case QEvent::MouseButtonPress:
	{
		auto* me = static_cast<QMouseEvent*>(event);
		m_dragHelper.mousePressed(me);
		m_pressedItem = m_list->itemAt(me->pos());
		break;
	}
	case QEvent::MouseMove:
	{
		if (!m_pressedItem)
			break;
		auto* me = static_cast<QMouseEvent*>(event);
		const QPixmap rowPixmap = m_list->viewport()->grab(m_list->visualItemRect(m_pressedItem));
		const bool started = m_dragHelper.tryStartDrag(
			m_list->viewport(), me,
			[this] { return m_makeMimeDataForRow(m_pressedItem); },
			Qt::CopyAction, rowPixmap);
		if (started)
		{
			m_pressedItem = nullptr;
			return true;
		}
		break;
	}
	case QEvent::MouseButtonRelease:
		m_pressedItem = nullptr;
		break;
	default:
		break;
	}

	return QObject::eventFilter(watched, event);
}
