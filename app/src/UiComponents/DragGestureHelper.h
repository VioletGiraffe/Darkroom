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

// Tracks a left-button press position and starts a QDrag once the mouse has moved
// past the system's drag-start threshold. Drive it from an eventFilter/mouse handler:
// call mousePressed() on MouseButtonPress, tryStartDrag() on MouseMove.
class DragGestureHelper
{
public:
	void mousePressed(const QMouseEvent* event);

	// Returns true if a drag was started (caller should treat the event as handled).
	// widget is the QDrag's parent. makeMimeData may return null to veto the drag - nothing starts and
	// false is returned. The drag pixmap is dragPixmap when given, otherwise the widget
	// grabbed whole (via grab()) - pass an explicit pixmap when only part of the widget should be shown
	// (e.g. a single list row). Either way it is capped to a maximum size (preserving aspect ratio) so
	// dragging large cards doesn't produce an oversized cursor.
	bool tryStartDrag(QWidget* widget, const QMouseEvent* moveEvent,
	                  const std::function<QMimeData*()>& makeMimeData, Qt::DropAction action,
	                  const QPixmap& dragPixmap = {});

private:
	QPoint _pressPos;
};

// The complete drag-out gesture for a QListWidget's rows, packaged as an event filter: construct it with the
// list and it installs itself on the list's viewport (and parents itself to the list, so its lifetime is
// automatic). A press-and-drag on a row starts a CopyAction QDrag carrying what the factory returns for that
// row, with the row itself (grabbed from the viewport) as the drag pixmap; the factory returns null for a
// row that must not drag. Plain clicks pass through untouched, so row click behavior is unaffected.
class ListRowDragFilter final : public QObject
{
public:
	// makeMimeDataForRow is called with the pressed row (never null) once the drag threshold is passed.
	ListRowDragFilter(QListWidget* list, std::function<QMimeData*(const QListWidgetItem*)> makeMimeDataForRow);

protected:
	bool eventFilter(QObject* watched, QEvent* event) override;

private:
	QListWidget* const _list;
	const std::function<QMimeData*(const QListWidgetItem*)> _makeMimeDataForRow;
	DragGestureHelper _dragHelper;
	QListWidgetItem* _pressedItem = nullptr;  // row under the last left-press, the drag's candidate source
};
