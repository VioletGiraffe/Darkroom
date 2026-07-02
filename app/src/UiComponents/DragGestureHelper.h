#pragma once

#include <QPixmap>
#include <QPoint>
#include <QSize>
#include <QWidget>

#include <functional>

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
	// widget is the QDrag's parent. The drag pixmap is dragPixmap when given, otherwise the widget
	// grabbed whole (via grab()) - pass an explicit pixmap when only part of the widget should be shown
	// (e.g. a single list row). Either way it is capped to a maximum size (preserving aspect ratio) so
	// dragging large cards doesn't produce an oversized cursor.
	bool tryStartDrag(QWidget* widget, const QMouseEvent* moveEvent,
	                  const std::function<QMimeData*()>& makeMimeData, Qt::DropAction action,
	                  const QPixmap& dragPixmap = {});

private:
	QPoint m_pressPos;
};
