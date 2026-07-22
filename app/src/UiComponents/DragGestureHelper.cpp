#include "UiComponents/DragGestureHelper.h"

#include <QApplication>
#include <QDrag>
#include <QListWidget>
#include <QMimeData>
#include <QMouseEvent>

void DragGestureHelper::mousePressed(const QMouseEvent* event)
{
	if (event->button() == Qt::LeftButton)
		_pressPos = event->pos();
}

static constexpr QSize MAX_DRAG_PIXMAP_SIZE{ 120, 120 };

bool DragGestureHelper::tryStartDrag(QWidget* widget, const QMouseEvent* moveEvent,
                                     const std::function<QMimeData*()>& makeMimeData, Qt::DropAction action,
                                     const QPixmap& dragPixmap)
{
	if (!(moveEvent->buttons() & Qt::LeftButton))
		return false;
	if ((moveEvent->pos() - _pressPos).manhattanLength() < QApplication::startDragDistance())
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
	, _list(list)
	, _makeMimeDataForRow(std::move(makeMimeDataForRow))
{
	_list->viewport()->installEventFilter(this);
}

bool ListRowDragFilter::eventFilter(QObject* watched, QEvent* event)
{
	switch (event->type())
	{
	case QEvent::MouseButtonPress:
	{
		auto* me = static_cast<QMouseEvent*>(event);
		_dragHelper.mousePressed(me);
		_pressedItem = _list->itemAt(me->pos());
		break;
	}
	case QEvent::MouseMove:
	{
		if (!_pressedItem)
			break;
		auto* me = static_cast<QMouseEvent*>(event);
		const QPixmap rowPixmap = _list->viewport()->grab(_list->visualItemRect(_pressedItem));
		const bool started = _dragHelper.tryStartDrag(
			_list->viewport(), me,
			[this] { return _makeMimeDataForRow(_pressedItem); },
			Qt::CopyAction, rowPixmap);
		if (started)
		{
			_pressedItem = nullptr;
			return true;
		}
		break;
	}
	case QEvent::MouseButtonRelease:
		_pressedItem = nullptr;
		break;
	default:
		break;
	}

	return QObject::eventFilter(watched, event);
}
