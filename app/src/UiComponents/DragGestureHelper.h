#pragma once

#include <QPixmap>
#include <QPoint>
#include <QSize>
#include <QWidget>

#include <functional>

class QListWidget;
class QListWidgetItem;
class QMimeData;
class QMouseEvent;

// Starts a drag after movement from the recorded left press crosses the system threshold.
class DragGestureHelper
{
public:
	void mousePressed(const QMouseEvent* event);

	// makeMimeData may veto by returning null. dragPixmap defaults to a grab of widget; either is size-capped.
	bool tryStartDrag(QWidget* widget, const QMouseEvent* moveEvent,
	                  const std::function<QMimeData*()>& makeMimeData, Qt::DropAction action,
	                  const QPixmap& dragPixmap = {});

private:
	QPoint _pressPos;
};

// Self-installing QListWidget row-drag filter. The factory may veto a row by returning null; clicks pass through.
class ListRowDragFilter final : public QObject
{
public:
	ListRowDragFilter(QListWidget* list, std::function<QMimeData*(const QListWidgetItem*)> makeMimeDataForRow);

protected:
	bool eventFilter(QObject* watched, QEvent* event) override;

private:
	QListWidget* const _list;
	const std::function<QMimeData*(const QListWidgetItem*)> _makeMimeDataForRow;
	DragGestureHelper _dragHelper;
	QListWidgetItem* _pressedItem = nullptr;
};
